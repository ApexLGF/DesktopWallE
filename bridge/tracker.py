"""
Bridge-side face tracker — drives 热狗's head to keep a detected face centered.

P5 architecture choice: instead of bolting esp-dl + a face model into the
firmware (significant size + complexity), we run OpenCV Haar-cascade
detection on the Mac mini using captures we already get for free over
ocsc.v2. Mac CPU is plenty fast for 320×240 detection (~5-15 fps), and
moving the inference off-device keeps the firmware lean.

Two public flows:

* `find_face(device, hub)`  — scan (sweep yaw, sample frames, detect face,
  step the head toward the largest hit). One-shot: returns when locked or
  scan exhausts.

* class `Tracker` — continuous loop that keeps the head aimed at the face
  it currently sees, retries on loss. Start/stop owned by the bridge.
"""
from __future__ import annotations

import asyncio
import io
import logging
import os
import time
from dataclasses import dataclass, field
from typing import Any

import cv2          # type: ignore
import numpy as np  # type: ignore
from PIL import Image

log = logging.getLogger("tracker")

_HAAR_FRONT_PATH = os.path.join(cv2.data.haarcascades,
                                  "haarcascade_frontalface_default.xml")
_HAAR_FRONT_ALT_PATH = os.path.join(cv2.data.haarcascades,
                                      "haarcascade_frontalface_alt2.xml")
_HAAR_PROFILE_PATH = os.path.join(cv2.data.haarcascades,
                                    "haarcascade_profileface.xml")
_haar_front: cv2.CascadeClassifier | None = None
_haar_front_alt: cv2.CascadeClassifier | None = None
_haar_profile: cv2.CascadeClassifier | None = None


def _load_cascade(path: str, cache: list[cv2.CascadeClassifier | None]) -> cv2.CascadeClassifier | None:
    if cache[0] is None:
        c = cv2.CascadeClassifier(path)
        if c.empty():
            log.warning("could not load cascade at %s", path)
            return None
        cache[0] = c
    return cache[0]


@dataclass
class Detection:
    x: int
    y: int
    w: int
    h: int
    @property
    def cx(self) -> int: return self.x + self.w // 2
    @property
    def cy(self) -> int: return self.y + self.h // 2
    @property
    def area(self) -> int: return self.w * self.h


_front_cache: list = [None]
_front_alt_cache: list = [None]
_profile_cache: list = [None]


def detect_faces_jpeg(jpeg: bytes) -> list[Detection]:
    """Multi-cascade face detection with histogram equalization.

    Tries (a) default frontal, (b) alt2 frontal (more permissive),
    (c) profile face — and once for the mirrored image so left-facing
    profiles also count. Histogram-equalises the grayscale image before
    detection so under-lit captures (the CoreS3 mic-side bezel is dark)
    still produce enough contrast. """
    img = Image.open(io.BytesIO(jpeg)).convert("L")
    arr = np.array(img)
    arr_eq = cv2.equalizeHist(arr)

    found: list[Detection] = []

    front = _load_cascade(_HAAR_FRONT_PATH, _front_cache)
    if front is not None:
        for (x, y, w, h) in front.detectMultiScale(
                arr_eq, scaleFactor=1.15, minNeighbors=3, minSize=(30, 30)):
            found.append(Detection(int(x), int(y), int(w), int(h)))

    if not found:
        alt = _load_cascade(_HAAR_FRONT_ALT_PATH, _front_alt_cache)
        if alt is not None:
            for (x, y, w, h) in alt.detectMultiScale(
                    arr_eq, scaleFactor=1.15, minNeighbors=3, minSize=(30, 30)):
                found.append(Detection(int(x), int(y), int(w), int(h)))

    if not found:
        profile = _load_cascade(_HAAR_PROFILE_PATH, _profile_cache)
        if profile is not None:
            for (x, y, w, h) in profile.detectMultiScale(
                    arr_eq, scaleFactor=1.15, minNeighbors=3, minSize=(30, 30)):
                found.append(Detection(int(x), int(y), int(w), int(h)))
            # Mirror for the other-facing profile (Haar profileface only
            # catches right-facing heads by default).
            arr_mirror = cv2.flip(arr_eq, 1)
            w_img = arr_mirror.shape[1]
            for (x, y, w, h) in profile.detectMultiScale(
                    arr_mirror, scaleFactor=1.15, minNeighbors=3, minSize=(30, 30)):
                # Reflect x back into the original image's coordinate space.
                found.append(Detection(int(w_img - x - w), int(y), int(w), int(h)))

    if found:
        log.info("face detect: %d hit(s) %s", len(found),
                 [(d.x, d.y, d.w, d.h) for d in found])
    return found


async def _capture_jpeg(hub: Any, device: Any) -> bytes | None:
    """Issue a cam_capture RPC and wait for the matching KIND_CAM_JPEG binary."""
    sid = device.next_sid()
    loop = asyncio.get_event_loop()
    fut: asyncio.Future = loop.create_future()
    device.pending_jpegs[sid] = fut
    try:
        await hub.rpc(device, "cam_capture", {"sid": sid, "quality": 60}, timeout=8.0)
        return await asyncio.wait_for(fut, timeout=4.0)
    except Exception as e:
        device.pending_jpegs.pop(sid, None)
        log.warning("tracker capture failed: %s", e)
        return None


# Frame is 320×240 with optical axis ~centered. Mapping pixel offset to head-
# angle delta is empirical — the head is close to a flat panel + a 60° FOV
# lens, so each pixel ≈ 0.2° of yaw / pitch. The StackChan angle unit is
# 0.3125° per step (units of 1/3.2 deg), so 1 pixel ≈ 0.6 angle units.
# We use a small gain (Kp=0.5) to avoid oscillation; settles in 2-3 steps.
KP_X = 0.5
KP_Y = 0.5


async def _step_toward(hub: Any, device: Any, face: Detection,
                       cur_x: int, cur_y: int, w: int = 320, h: int = 240) -> tuple[int, int]:
    dx = face.cx - w // 2
    dy = face.cy - h // 2
    new_x = int(round(cur_x + dx * KP_X * 0.6 * -1))   # camera mirror: +x_px = +yaw_left
    new_y = int(round(cur_y + dy * KP_Y * 0.6))
    new_x = max(-1280, min(1280, new_x))
    new_y = max(30,    min(870,  new_y))
    if new_x == cur_x and new_y == cur_y:
        return cur_x, cur_y
    log.info("step face@(%d,%d) head (%d,%d) -> (%d,%d)",
             face.cx, face.cy, cur_x, cur_y, new_x, new_y)
    try:
        await hub.rpc(device, "move_head", {"x": new_x, "y": new_y, "speed": 400}, timeout=2.0)
    except Exception as e:
        log.warning("move_head failed: %s", e)
    return new_x, new_y


SCAN_WAYPOINTS = [
    (0, 400), (-600, 400), (-300, 400), (300, 400),
    (600, 400), (0, 300), (0, 500),
]


async def find_face(hub: Any, device: Any, max_steps: int = 12) -> dict:
    """Scan a handful of head poses, detect faces in each frame, step toward
    the largest. Returns the final pose + best face once locked, or {locked:
    false} if nothing found within `max_steps`."""
    cur_x, cur_y = 0, 400
    best_seen: Detection | None = None
    for wpt_x, wpt_y in SCAN_WAYPOINTS:
        if cur_x != wpt_x or cur_y != wpt_y:
            try:
                await hub.rpc(device, "move_head", {"x": wpt_x, "y": wpt_y, "speed": 500},
                              timeout=3.0)
            except Exception as e:
                log.warning("scan move failed: %s", e)
                continue
            cur_x, cur_y = wpt_x, wpt_y
            await asyncio.sleep(0.4)                    # let the head settle
        jpeg = await _capture_jpeg(hub, device)
        if jpeg is None:
            continue
        faces = detect_faces_jpeg(jpeg)
        if not faces:
            continue
        big = max(faces, key=lambda f: f.area)
        log.info("face seen @ pose (%d,%d): bbox=(%d,%d,%d,%d) area=%d",
                 cur_x, cur_y, big.x, big.y, big.w, big.h, big.area)
        best_seen = big
        # nudge a few times to center on this face
        for _ in range(3):
            jpeg = await _capture_jpeg(hub, device)
            if jpeg is None: break
            faces = detect_faces_jpeg(jpeg)
            if not faces: break
            big = max(faces, key=lambda f: f.area)
            best_seen = big
            offset = (abs(big.cx - 160) + abs(big.cy - 120))
            if offset < 30:                              # close enough
                return {"locked": True, "x": cur_x, "y": cur_y,
                        "face": {"cx": big.cx, "cy": big.cy, "w": big.w, "h": big.h}}
            cur_x, cur_y = await _step_toward(hub, device, big, cur_x, cur_y)
            await asyncio.sleep(0.25)
        return {"locked": True, "x": cur_x, "y": cur_y,
                "face": {"cx": best_seen.cx, "cy": best_seen.cy,
                         "w": best_seen.w, "h": best_seen.h}}
    return {"locked": False, "x": cur_x, "y": cur_y}


@dataclass
class Tracker:
    hub: Any
    device: Any
    period_s: float = 0.4
    _task: asyncio.Task | None = None
    _stop: bool = False
    cur_x: int = 0
    cur_y: int = 400

    async def _loop(self):
        misses = 0
        while not self._stop:
            jpeg = await _capture_jpeg(self.hub, self.device)
            if jpeg is None:
                await asyncio.sleep(self.period_s)
                continue
            faces = detect_faces_jpeg(jpeg)
            if not faces:
                misses += 1
                if misses >= 8:                          # ~3s of nothing → rescan
                    log.info("track: lost face, rescanning")
                    res = await find_face(self.hub, self.device)
                    self.cur_x = res.get("x", self.cur_x)
                    self.cur_y = res.get("y", self.cur_y)
                    misses = 0
                await asyncio.sleep(self.period_s)
                continue
            misses = 0
            big = max(faces, key=lambda f: f.area)
            self.cur_x, self.cur_y = await _step_toward(
                self.hub, self.device, big, self.cur_x, self.cur_y)
            await asyncio.sleep(self.period_s)

    def start(self):
        if self._task is not None and not self._task.done():
            return
        self._stop = False
        loop = asyncio.get_event_loop()
        self._task = loop.create_task(self._loop())

    def stop(self):
        self._stop = True

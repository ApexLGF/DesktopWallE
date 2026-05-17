"""
bridge MCP server — exposes 热狗 device capabilities to Hermes as MCP tools.

Runs alongside `server.py` as a second process. The WS server inside server.py
owns the device long-connection and admin HTTP (/healthz, /status, /rpc).
This process speaks MCP (streamable-http on :8767, path /mcp) and shells RPCs
back through bridge admin HTTP. Decoupling means: a crash here doesn't drop
the device link, and Hermes can be pointed at /mcp without knowing the WS
internals.

Tools wired in P2c (sync, RPC-direct):
  hotdog_move_head, hotdog_do_action, hotdog_set_led, hotdog_get_sensors,
  hotdog_ping.
Streaming tools (say/show_image/take_photo/find_face/track_*) land in P3-P5.

Run:  python3 mcp_server.py
"""
from __future__ import annotations

import argparse
import logging
import os
from typing import Any

import httpx
from mcp.server.fastmcp import FastMCP

log = logging.getLogger("mcp")

BRIDGE_URL = os.environ.get("HOTDOG_BRIDGE", "http://127.0.0.1:8766")


async def _say(text: str, *, voice: str | None = None,
               sample_rate: int = 16000, speed: float = 1.0,
               timeout: float = 30.0) -> dict[str, Any]:
    body: dict[str, Any] = {"text": text, "sample_rate": sample_rate, "speed": speed}
    if voice:
        body["voice"] = voice
    async with httpx.AsyncClient(timeout=timeout + 5) as c:
        r = await c.post(f"{BRIDGE_URL}/say", json=body)
        data = r.json()
    if not data.get("ok"):
        raise RuntimeError(data.get("error", "say_failed"))
    return {"bytes": data.get("bytes"), "sample_rate": data.get("sample_rate"),
            "est_ms": data.get("est_ms")}


async def _rpc(method: str, params: dict | None = None, timeout: float = 6.0) -> dict[str, Any]:
    body: dict[str, Any] = {"method": method, "timeout": timeout}
    if params is not None:
        body["params"] = params
    async with httpx.AsyncClient(timeout=timeout + 4) as c:
        r = await c.post(f"{BRIDGE_URL}/rpc", json=body)
        try:
            data = r.json()
        except Exception:
            r.raise_for_status()
            raise
    if not data.get("ok"):
        raise RuntimeError(data.get("error", "rpc_failed"))
    return data.get("result") or {}


def build_app() -> FastMCP:
    mcp = FastMCP(
        "hotdog",
        instructions=(
            "热狗 is a StackChan robot. These tools drive its head, RGB LEDs, "
            "and onboard sensors over a long-lived WebSocket. Coordinates use "
            "the StackChan motion units (yaw [-1280..1280], pitch [30..870]). "
            "Camera, voice, and face-tracking tools are added in later phases."
        ),
        host=os.environ.get("HOTDOG_MCP_HOST", "0.0.0.0"),
        port=int(os.environ.get("HOTDOG_MCP_PORT", "8767")),
        stateless_http=True,
    )

    @mcp.tool()
    async def hotdog_ping() -> dict:
        """Liveness check — confirms the device is online and the WS RPC works."""
        return await _rpc("ping")

    @mcp.tool()
    async def hotdog_move_head(yaw: int = 0, pitch: int = 400, speed: int = 300) -> dict:
        """Move the head to (yaw, pitch).

        yaw: -1280..1280 (negative = look left, positive = look right)
        pitch: 30..870 (small = look up, ~400 = neutral, large = look down)
        speed: 0..1000 (0 = servo default)
        Returns the clamped (x, y) the device actually committed to.
        """
        return await _rpc("move_head", {"x": yaw, "y": pitch, "speed": speed})

    @mcp.tool()
    async def hotdog_do_action(name: str) -> dict:
        """Run a predefined head gesture.

        name: home | nod | shake | left | right | look_up | look_down
        """
        return await _rpc("do_action", {"name": name}, timeout=8.0)

    @mcp.tool()
    async def hotdog_set_led(r: int = 0, g: int = 0, b: int = 0,
                              index: int | None = None) -> dict:
        """Set the neon ring LEDs.

        Without index: all 12 LEDs to (r, g, b).
        With index 0..11: just that LED. Each channel 0..255.
        """
        params: dict[str, Any] = {"r": r, "g": g, "b": b}
        if index is not None:
            params["index"] = index
        return await _rpc("set_led", params)

    @mcp.tool()
    async def hotdog_say(text: str, speed: float = 1.0) -> dict:
        """Speak `text` out the device speaker (Doubao TTS, Chinese/English).

        Returns the estimated playback duration in ms. The call returns once the
        audio has been streamed to the device — the device keeps playing for
        ~est_ms milliseconds after the call returns.
        speed: 0.5..2.0, 1.0 = normal.
        """
        return await _say(text, speed=speed)

    @mcp.tool()
    async def hotdog_perform(text: str, cues: list[dict] | None = None,
                             speed: float = 1.0) -> dict:
        """Recite `text` continuously (one Doubao TTS call) while firing
        scheduled gesture/LED/face cues on the bridge's clock.

        Use this instead of looping `hotdog_say` per line — that creates
        synth-roundtrip gaps and 1.2 s motion blocks the audio. `/perform`
        synthesizes the whole text once and runs cues concurrently with
        playback.

        `cues` is a list like:
          [{"at_ms": 0,
            "led": {"r":255,"g":80,"b":0},
            "motion": "look_up",
            "text": "君不见黄河之水天上来"},
           {"at_ms": 4000,
            "led": {"r":0,"g":200,"b":255},
            "motion": "nod",
            "text": "君不见高堂明镜悲白发"}]

        Per-cue fields (all optional):
          - `led`:   {r,g,b}              flash the neon ring
          - `motion`: nod/shake/left/right/look_up/look_down/home
          - `text`:  subtitle line on the LCD (bridge renders CJK with PIL)
          - `title`: small heading above subtitle
          - `face`:  built-in face icon inset with the subtitle

        For a 13-line poem with ~3.5-4 s per line, each cue's at_ms is
        the cumulative line duration. After the last cue + ~1.5 s the
        bridge automatically restores the default `idle` face.
        """
        body: dict[str, Any] = {"text": text, "speed": speed}
        if cues:
            body["cues"] = cues
        async with httpx.AsyncClient(timeout=180.0) as c:
            r = await c.post(f"{BRIDGE_URL}/perform", json=body)
            data = r.json()
        if not data.get("ok"):
            raise RuntimeError(data.get("error", "perform_failed"))
        return {"bytes": data["bytes"], "est_ms": data["est_ms"],
                "cues_fired": data.get("cues_fired", 0)}

    @mcp.tool()
    async def hotdog_show_image(url: str | None = None, jpeg_b64: str | None = None) -> dict:
        """Display an image full-screen on 热狗's 320x240 LCD.

        Pass `url` (an http(s) URL to fetch) OR `jpeg_b64` (a base64-encoded
        image — any format Pillow can decode). The bridge resizes/letterboxes
        to 320x240, JPEG-encodes, and streams to the device.
        """
        if not (url or jpeg_b64):
            raise RuntimeError("provide either url= or jpeg_b64=")
        if url:
            async with httpx.AsyncClient(timeout=20.0, follow_redirects=True) as c:
                r = await c.get(url)
                r.raise_for_status()
                body = r.content
        else:
            import base64
            body = base64.b64decode(jpeg_b64)
        async with httpx.AsyncClient(timeout=20.0) as c:
            r = await c.post(f"{BRIDGE_URL}/show_image", content=body,
                             headers={"Content-Type": "application/octet-stream"})
            data = r.json()
        if not data.get("ok"):
            raise RuntimeError(data.get("error", "show_image_failed"))
        return {"bytes": data["bytes"]}

    @mcp.tool()
    async def hotdog_face(expr: str = "idle") -> dict:
        """Show a built-in robot face on 热狗's 320x240 LCD.

        Rendered on the bridge with PIL (cute eyes + mouth vector art) and
        shipped as one JPEG. Cached per-expression, so repeated calls are
        instant.

        `expr` ∈ idle | happy | sad | surprised | talking | listening |
                  sleepy | blink | thinking | alert

        Anything else falls back to `idle`. After every `hotdog_perform`
        the bridge auto-restores `idle` ~1.5 s after the last cue, so you
        don't need to do that yourself."""
        async with httpx.AsyncClient(timeout=10.0) as c:
            r = await c.post(f"{BRIDGE_URL}/face", json={"expr": expr})
            return r.json()

    @mcp.tool()
    async def hotdog_show_text(text: str, title: str = "",
                                face: str | None = None,
                                size: str = "auto") -> dict:
        """Show text on 热狗's LCD. CJK and English both work.

        - `text`: main body (multi-line OK, the bridge wraps it).
        - `title`: optional small heading above the body.
        - `face`: optional name of a built-in face icon to inset
          upper-left (e.g. "happy"). Skip for full-width text.
        - `size`: small | medium | large | auto. `auto` picks based on
          character count.

        Subtitle-style usage during recitation is built into
        `hotdog_perform` — pass `text` per cue and the bridge ships it
        at the cue's at_ms. Use this tool for one-off announcements."""
        body: dict[str, Any] = {"text": text, "title": title, "size": size}
        if face:
            body["face"] = face
        async with httpx.AsyncClient(timeout=10.0) as c:
            r = await c.post(f"{BRIDGE_URL}/show_text", json=body)
            return r.json()

    @mcp.tool()
    async def hotdog_find_face(max_steps: int = 12) -> dict:
        """Sweep the head, find a face, lock onto it.

        Scans through a handful of head poses, runs OpenCV face detection on
        each captured frame, then nudges the head to center on the largest
        face found. Returns once locked (offset < 30 px) or scan exhausted.
        On success: {locked: true, x, y, face: {cx, cy, w, h}}.
        Useful prelude to `hotdog_take_photo` for the "take a photo of me" flow.
        """
        async with httpx.AsyncClient(timeout=45.0) as c:
            r = await c.post(f"{BRIDGE_URL}/find_face",
                             json={"max_steps": max_steps})
            return r.json()

    @mcp.tool()
    async def hotdog_track_start() -> dict:
        """Start continuous face tracking — the head keeps aiming at the
        nearest detected face, re-scanning if it loses sight. Runs as a
        bridge-side task at ~2.5 fps. Don't enable during long agent loops
        that need stable head pose."""
        async with httpx.AsyncClient(timeout=10.0) as c:
            r = await c.post(f"{BRIDGE_URL}/track", json={"action": "start"})
            return r.json()

    @mcp.tool()
    async def hotdog_track_stop() -> dict:
        """Stop continuous face tracking."""
        async with httpx.AsyncClient(timeout=10.0) as c:
            r = await c.post(f"{BRIDGE_URL}/track", json={"action": "stop"})
            return r.json()

    @mcp.tool()
    async def hotdog_take_photo(quality: int = 70) -> dict:
        """Snap a JPEG from the device's onboard camera (320x240 GC0308).

        Returns the photo as base64-encoded JPEG bytes plus dimensions.
        `quality` 1..100 (higher = larger file, default 70 ≈ 20 KB).
        """
        async with httpx.AsyncClient(timeout=20.0) as c:
            r = await c.post(f"{BRIDGE_URL}/capture",
                             json={"quality": quality, "format": "json"})
            data = r.json()
        if not data.get("ok"):
            raise RuntimeError(data.get("error", "capture_failed"))
        return {"w": data["w"], "h": data["h"], "bytes": data["bytes"],
                "jpeg_b64": data["jpeg_b64"]}

    @mcp.tool()
    async def hotdog_get_sensors() -> dict:
        """Read battery percent, free heap, boot count, and wifi state."""
        return await _rpc("get_sensors")

    return mcp


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--log-level", default="INFO")
    args = ap.parse_args()
    logging.basicConfig(
        level=getattr(logging, args.log_level.upper()),
        format="%(asctime)s %(levelname)s %(name)s %(message)s",
    )
    app = build_app()
    log.info("starting MCP server (streamable-http) on %s:%d /mcp -> bridge %s",
             app.settings.host, app.settings.port, BRIDGE_URL)
    app.run(transport="streamable-http")


if __name__ == "__main__":
    main()

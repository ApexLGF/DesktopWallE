"""
ocsc.v2 — 热狗 device <-> bridge WebSocket sub-protocol.

Text frames: JSON control/RPC/events. Binary frames: 8-byte header + payload
(audio PCM, camera JPEG, display JPEG). See
~/.claude/plans/hotdog-firmware-replacement.md for the full spec.

Ported from stackproxy's ocsc.v1 protocol.py — frame shapes are unchanged;
v2 bumps the binary header version byte and adds the camera/display kinds.
"""
from __future__ import annotations

import json
import struct
from dataclasses import dataclass
from typing import Any

PROTOCOL_VERSION = 0x02

# binary frame "kind" bytes
KIND_MIC_PCM = 0x01    # device -> bridge : PCM16LE 16k mono mic
KIND_TTS_PCM = 0x02    # bridge -> device : PCM16LE 16k mono speaker
KIND_CAM_JPEG = 0x03   # device -> bridge : JPEG capture result
KIND_DISP_IMG = 0x04   # bridge -> device : JPEG to show full-screen
KIND_DISP_RGB565 = 0x05  # bridge -> device : raw RGB565 LE 320x240 = 153600 B

# 8-byte header: ver(1) + kind(1) + sid(uint16 LE) + seq(uint32 LE)
BINARY_HEADER_FMT = "<BBHI"
BINARY_HEADER_LEN = struct.calcsize(BINARY_HEADER_FMT)  # 8


@dataclass
class BinaryFrame:
    version: int
    kind: int
    sid: int
    seq: int
    payload: bytes


def encode_binary(kind: int, sid: int, seq: int, payload: bytes) -> bytes:
    return struct.pack(BINARY_HEADER_FMT, PROTOCOL_VERSION, kind, sid, seq) + payload


def decode_binary(data: bytes) -> BinaryFrame | None:
    if len(data) < BINARY_HEADER_LEN:
        return None
    ver, kind, sid, seq = struct.unpack(BINARY_HEADER_FMT, data[:BINARY_HEADER_LEN])
    return BinaryFrame(ver, kind, sid, seq, data[BINARY_HEADER_LEN:])


# ---------- text frames ----------

def encode_text(obj: dict[str, Any]) -> str:
    return json.dumps(obj, ensure_ascii=False, separators=(",", ":"))


def decode_text(raw: str) -> dict[str, Any] | None:
    try:
        obj = json.loads(raw)
    except (json.JSONDecodeError, TypeError):
        return None
    return obj if isinstance(obj, dict) else None


# ---------- convenience builders ----------

def frame_req(rpc_id: str, method: str, params: dict | None = None) -> str:
    obj: dict[str, Any] = {"t": "req", "id": rpc_id, "m": method}
    if params is not None:
        obj["p"] = params
    return encode_text(obj)


def frame_err(rpc_id: str, code: str, msg: str = "") -> str:
    return encode_text({"t": "err", "id": rpc_id, "code": code, "msg": msg})


def frame_evt(name: str, data: dict | None = None) -> str:
    obj: dict[str, Any] = {"t": "evt", "name": name}
    if data is not None:
        obj["d"] = data
    return encode_text(obj)


def frame_pong(ts: int = 0) -> str:
    return encode_text({"t": "pong", "ts": ts})


def frame_hello_ack(ts: int) -> str:
    return encode_text({"t": "hello.ack", "ts": ts})


def frame_mic_start(sid: int, sr: int = 16000) -> str:
    return encode_text({"t": "mic.start", "sid": sid, "sr": sr, "fmt": "pcm16"})


def frame_mic_end(sid: int, reason: str = "") -> str:
    return encode_text({"t": "mic.end", "sid": sid, "reason": reason})


def frame_tts_start(sid: int, sr: int = 16000, est_ms: int = 0) -> str:
    return encode_text({"t": "tts.start", "sid": sid, "sr": sr,
                        "fmt": "pcm16", "est_ms": est_ms})


def frame_tts_end(sid: int) -> str:
    return encode_text({"t": "tts.end", "sid": sid})


def frame_disp_image(image_id: int, nbytes: int, fmt: str = "jpeg") -> str:
    return encode_text({"t": "disp.image", "id": image_id, "fmt": fmt, "bytes": nbytes})


def frame_disp_anim(image_id: int, frames: int, fps: int = 8,
                    loop: bool = True, fmt: str = "jpeg") -> str:
    return encode_text({"t": "disp.anim", "id": image_id, "fmt": fmt,
                        "frames": frames, "fps": fps, "loop": loop})


def frame_disp_text(title: str, text: str) -> str:
    return encode_text({"t": "disp.text", "title": title, "text": text})


def frame_disp_face(expr: str) -> str:
    return encode_text({"t": "disp.face", "expr": expr})

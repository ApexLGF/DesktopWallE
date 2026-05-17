"""
Doubao (Volcengine) TTS — REST one-shot.

We hit POST https://openspeech.bytedance.com/api/v1/tts with operation=query,
get back a JSON envelope with a base64-encoded PCM16 16 kHz mono payload,
and return raw PCM bytes. Streaming happens on the device side via the bridge
ocsc.v2 binary path (KIND_TTS_PCM); the device speaker handles the playback
buffer, so the bridge only needs to chunk the bytes nicely.

Credentials live in ~/.stackproxy/config.toml (carried over from stackproxy),
or env: HOTDOG_DOUBAO_APPID / _ACCESS_TOKEN.

Docs: https://www.volcengine.com/docs/6561/79820
"""
from __future__ import annotations

import base64
import logging
import os
import uuid
from pathlib import Path

import httpx

log = logging.getLogger("doubao_tts")

DEFAULT_VOICE = "BV001_streaming"          # 通用女声,流式优化版
DEFAULT_CLUSTER = "volcano_tts"
TTS_ENDPOINT = "https://openspeech.bytedance.com/api/v1/tts"


def _load_creds() -> tuple[str, str]:
    appid = os.environ.get("HOTDOG_DOUBAO_APPID")
    token = os.environ.get("HOTDOG_DOUBAO_ACCESS_TOKEN")
    if appid and token:
        return appid, token
    # Fallback: pull from the stackproxy config.toml the user already maintains.
    cfg = Path.home() / ".stackproxy" / "config.toml"
    if cfg.exists():
        try:
            import tomllib
        except ModuleNotFoundError:
            import tomli as tomllib  # type: ignore
        with cfg.open("rb") as f:
            data = tomllib.load(f)
        d = data.get("doubao", {})
        appid = d.get("appid", appid)
        token = d.get("access_token", token)
    if not appid or not token:
        raise RuntimeError("Doubao TTS credentials missing — set HOTDOG_DOUBAO_APPID + "
                           "HOTDOG_DOUBAO_ACCESS_TOKEN or fill in ~/.stackproxy/config.toml")
    return appid, token


async def synthesize(text: str, *, voice: str = DEFAULT_VOICE,
                     sample_rate: int = 16000, speed: float = 1.0) -> bytes:
    """Synthesize `text` to raw PCM16 mono bytes at `sample_rate` Hz."""
    appid, token = _load_creds()
    body = {
        "app": {"appid": appid, "token": token, "cluster": DEFAULT_CLUSTER},
        "user": {"uid": "hotdog"},
        "audio": {
            "voice_type": voice,
            "encoding": "pcm",
            "rate": sample_rate,
            "speed_ratio": speed,
        },
        "request": {
            "reqid": uuid.uuid4().hex,
            "text": text,
            "operation": "query",
        },
    }
    headers = {
        "Authorization": f"Bearer;{token}",        # NB: semicolon, not space — Doubao quirk
        "Content-Type": "application/json",
    }
    async with httpx.AsyncClient(timeout=30) as c:
        r = await c.post(TTS_ENDPOINT, json=body, headers=headers)
    if r.status_code != 200:
        raise RuntimeError(f"Doubao TTS HTTP {r.status_code}: {r.text[:200]}")
    data = r.json()
    if data.get("code") != 3000:
        raise RuntimeError(f"Doubao TTS error {data.get('code')}: {data.get('message')}")
    audio_b64 = data.get("data") or ""
    if not audio_b64:
        raise RuntimeError("Doubao TTS returned empty audio")
    pcm = base64.b64decode(audio_b64)
    log.info("synthesized %d bytes pcm (%s)", len(pcm), text[:60])
    return pcm

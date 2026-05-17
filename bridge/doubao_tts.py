"""
Doubao (Volcengine) TTS — REST one-shot.

We hit POST https://openspeech.bytedance.com/api/v1/tts with operation=query,
get back a JSON envelope with a base64-encoded PCM16 16 kHz mono payload,
and return raw PCM bytes. Streaming happens on the device side via the bridge
ocsc.v2 binary path (KIND_TTS_PCM); the device speaker handles the playback
buffer, so the bridge only needs to chunk the bytes nicely.

Credentials are loaded from bridge/config.toml via bridge.config (see
bridge/config.toml.example for the schema). There are no env-var fallbacks.

Docs: https://www.volcengine.com/docs/6561/79820
"""
from __future__ import annotations

import base64
import logging
import uuid

import httpx

import config as bridge_config

log = logging.getLogger("doubao_tts")

DEFAULT_VOICE = "BV001_streaming"          # 通用女声,流式优化版
DEFAULT_CLUSTER = "volcano_tts"
TTS_ENDPOINT = "https://openspeech.bytedance.com/api/v1/tts"


def _load_creds() -> tuple[str, str]:
    """Return (appid, access_token) from bridge/config.toml.

    Kept as a thin shim because half the codebase calls _load_creds() —
    new code should call bridge_config.get_doubao() directly.
    """
    c = bridge_config.get_doubao()
    return c.appid, c.access_token


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

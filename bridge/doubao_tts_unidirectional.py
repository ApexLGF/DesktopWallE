"""Doubao TTS unidirectional HTTP Chunked client.

Endpoint: https://openspeech.bytedance.com/api/v3/tts/unidirectional
Resource: volc.service_type.10029 (legacy console TTS V3 unidirectional)

Wire format (verified live 2026-05-17):
  - HTTP/1.1 POST with `Content-Type: application/json`, body is V3
    `req_params` payload (user / req_params.text / speaker / audio_params).
  - Response is `Content-Type: text/plain; charset=utf-8` with
    `Transfer-Encoding: chunked` and `Content-Encoding: gzip` (httpx
    auto-decompresses).
  - Each chunk body is a single line of JSON:
        {"code":0, "message":"", "data": "<base64-encoded PCM>"}        # audio frame
        {"code":0, "message":"", "sentence":{"text":"...", "phonemes":[], "words":[]}}  # sentence end
  - Stream ends with the connection closing naturally (no sentinel).
  - For 22-character Chinese: first audio chunk ≈812 ms after POST, total
    144 KB PCM (≈4.5 s of 16 kHz mono int16) delivered in ~1.5 s wall time.

Why HTTP Chunked instead of WebSocket?
  - Same backend, no protocol-framing complexity (no binary frames, no
    seq numbers, no event codes — just JSON lines).
  - `httpx.AsyncClient.stream` + `aiter_lines()` does all the work.
  - Connection reuse via a shared AsyncClient saves ~70 ms per call.

Usage:
    async for pcm_chunk in synthesize_stream("你好", app_key=..., access_key=...):
        device.send(pcm_chunk)
"""
from __future__ import annotations

import asyncio
import base64
import json
import logging
import time
import uuid
from collections.abc import AsyncIterator
from dataclasses import dataclass

import httpx

log = logging.getLogger("doubao_tts_uni")

URL          = "https://openspeech.bytedance.com/api/v3/tts/unidirectional"
RESOURCE_ID  = "volc.service_type.10029"

# Default speaker — neutral 普通女声 fast-emotion (suitable for 热狗 robot).
# Switch via `speaker=` kwarg. Common alternatives:
#   zh_female_qingxin             — 清新女声
#   zh_male_jieshuo_bigtts         — 解说男声
#   BV701_streaming                — TTS 1.0 流式（兼容老接口）
DEFAULT_SPEAKER = "zh_female_shuangkuaisisi_moon_bigtts"


@dataclass
class StreamStats:
    first_audio_ms: int   # latency from request to first audio chunk
    total_audio_bytes: int
    total_chunks: int
    duration_ms: int      # wall-clock total
    sentences: list[str]  # text echoes from {"sentence":{...}} markers


class TTSError(RuntimeError):
    """Doubao TTS server returned a non-zero code in one of the JSON lines."""
    def __init__(self, code: int, message: str, raw: dict | None = None):
        super().__init__(f"tts error code={code} msg={message}")
        self.code = code
        self.message = message
        self.raw = raw


async def synthesize_stream(
    text: str,
    *,
    app_key: str,
    access_key: str,
    speaker: str = DEFAULT_SPEAKER,
    sample_rate: int = 16000,
    audio_format: str = "pcm",
    uid: str = "stackchan",
    speech_rate: int = 0,        # [-50, 100]   100 = 2× speed, -50 = 0.5×
    loudness_rate: int = 0,
    timeout_s: float = 30.0,
    http_client: httpx.AsyncClient | None = None,
    stats: StreamStats | None = None,
) -> AsyncIterator[bytes]:
    """Stream raw PCM bytes from Doubao TTS.

    Args:
        text: Chinese / English text to synthesise. Empty → 45000001.
        speaker: voice id, see DEFAULT_SPEAKER comment for options.
        sample_rate: 8000/16000/24000/32000/44100/48000. Doubao docs §audio.
        audio_format: 'pcm' (recommended for streaming) / 'mp3' / 'ogg_opus'.
            'wav' is broken for streaming (header repeats per chunk).
        speech_rate / loudness_rate: -50 to +100 (-50 = half, 100 = 2×).
        timeout_s: hard cap on the streaming request.
        http_client: pre-existing AsyncClient for connection reuse (cuts
            ~70 ms TLS per call).
        stats: optional StreamStats to receive timing/metadata.

    Yields:
        bytes — base64-decoded PCM chunks (each typically 6–8 KB).

    Raises:
        TTSError on non-zero `code` field from server.
        httpx.RequestError on network failure.
    """
    body = {
        "user": {"uid": uid},
        "req_params": {
            "text":    text,
            "speaker": speaker,
            "audio_params": {
                "format":        audio_format,
                "sample_rate":   sample_rate,
                "speech_rate":   speech_rate,
                "loudness_rate": loudness_rate,
            },
        },
    }
    headers = {
        "X-Api-App-Id":      app_key,
        "X-Api-Access-Key":  access_key,
        "X-Api-Resource-Id": RESOURCE_ID,
        "X-Api-Request-Id":  str(uuid.uuid4()),
        "Content-Type":      "application/json",
    }

    t0 = time.monotonic()
    first_audio_ms: int | None = None
    total_bytes = 0
    total_chunks = 0
    sentences: list[str] = []

    own_client = http_client is None
    client = http_client or httpx.AsyncClient(timeout=timeout_s)
    try:
        async with client.stream("POST", URL, headers=headers, json=body, timeout=timeout_s) as r:
            logid = r.headers.get("X-Tt-Logid") or ""
            log.info("tts open: http=%d ct=%s logid=%s",
                     r.status_code, r.headers.get("content-type"), logid)
            if r.status_code != 200:
                # body is small JSON
                raw = await r.aread()
                raise TTSError(r.status_code, f"http {r.status_code}: {raw[:200]!r}")
            async for line in r.aiter_lines():
                if not line:
                    continue
                total_chunks += 1
                try:
                    j = json.loads(line)
                except json.JSONDecodeError:
                    log.warning("tts non-json chunk: %r", line[:120])
                    continue
                code = j.get("code", 0)
                msg  = j.get("message", "")
                if code not in (0, 20000000):
                    raise TTSError(code, msg, j)
                data_b64 = j.get("data") or ""
                if data_b64:
                    pcm = base64.b64decode(data_b64)
                    total_bytes += len(pcm)
                    if first_audio_ms is None:
                        first_audio_ms = int((time.monotonic() - t0) * 1000)
                        log.info("tts first audio +%dms (%d B)", first_audio_ms, len(pcm))
                    yield pcm
                # sentence-end marker (no audio)
                sentence = j.get("sentence") or {}
                if sentence.get("text"):
                    sentences.append(sentence["text"])
    finally:
        if own_client:
            await client.aclose()
        if stats is not None:
            stats.first_audio_ms = first_audio_ms or 0
            stats.total_audio_bytes = total_bytes
            stats.total_chunks = total_chunks
            stats.duration_ms = int((time.monotonic() - t0) * 1000)
            stats.sentences = sentences
        log.info("tts done: %d chunks %d B audio in %dms (first=%dms)",
                 total_chunks, total_bytes,
                 int((time.monotonic() - t0) * 1000),
                 first_audio_ms or -1)


async def synthesize_all(text: str, **kwargs) -> bytes:
    """Convenience: concatenate the entire streamed PCM into one buffer."""
    chunks: list[bytes] = []
    async for c in synthesize_stream(text, **kwargs):
        chunks.append(c)
    return b"".join(chunks)

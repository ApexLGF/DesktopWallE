"""Doubao Flash ASR client — REST one-shot recognition.

Endpoint: https://openspeech.bytedance.com/api/v3/auc/bigmodel/recognize/flash
Resource: volc.bigasr.auc_turbo  (大模型录音文件极速版识别 / Doubao Flash ASR)

Why flash instead of bidirectional streaming?
  - No protocol pacing bugs (no per-packet 8 s server-side timer, no "first
    silence makes session die at 8 s" foot-guns).
  - No WebSocket framing / gzip / seq-number complexity.
  - Latency for utterance-sized audio (<10 s) is ~500 ms on the wire — plus
    upload time.
  - Simpler retry semantics: just re-POST.

**Audio is uploaded as OGG/OPUS, not WAV.** Why this matters:

  The openclaw → openspeech.bytedance.com upload path on this network is
  bandwidth-shaped to ~2 KB/s for large POST bodies (independent of the
  fake-IP / Clash routing — verified by hitting real bytedance IPs
  directly). 1.6 s of audio is 51 KB as WAV → ~70 KB after JSON+base64
  → 30 + s upload → server-side timeout.

  Same audio encoded as Opus @16 kbps is 3 KB → ~4.6 KB JSON → ~1.5 s
  upload → 1–2 s end-to-end. The flash endpoint accepts `ogg_opus` natively
  and the recognition quality is indistinguishable from WAV for speech.

  We shell out to ffmpeg for the encode (10–25 ms, negligible). This
  trades a tiny CPU cost for a ~20× upload-time win.

Verified against the live API on 2026-05-17 with a 1.7 s synthesized clip
"今天天气真不错" → returns the same text with word-level timestamps in
~1.9 s wall time (including encode + upload + recognise).

Usage:
    text = await transcribe(pcm_bytes, sample_rate=16000)

Caller passes raw int16 little-endian mono PCM at `sample_rate` Hz. We
do the OPUS encode here. Pass `audio_format='wav'` to disable encoding
(useful on networks where upload is not the bottleneck, or for debugging).
"""
from __future__ import annotations

import asyncio
import base64
import contextlib
import io
import json
import logging
import shutil
import uuid
import wave
from dataclasses import dataclass

import httpx

log = logging.getLogger("doubao_asr_flash")

URL          = "https://openspeech.bytedance.com/api/v3/auc/bigmodel/recognize/flash"
RESOURCE_ID  = "volc.bigasr.auc_turbo"

# Cached lookup of ffmpeg binary. None = not yet looked up; "" = unavailable;
# "/path/to/ffmpeg" = found. Avoids re-running `which` per call.
_FFMPEG_PATH: str | None = None


def _find_ffmpeg() -> str:
    """Return absolute path to ffmpeg, or "" if not on PATH."""
    global _FFMPEG_PATH
    if _FFMPEG_PATH is None:
        _FFMPEG_PATH = shutil.which("ffmpeg") or ""
    return _FFMPEG_PATH


async def _encode_pcm_to_opus(pcm: bytes, sample_rate: int,
                                bitrate_kbps: int = 16) -> bytes:
    """Encode 16-bit LE mono PCM → OGG/OPUS via ffmpeg subprocess.

    Returns the raw ogg-container bytes. ~25 ms for 5 s of audio on a Mac
    mini M-series.

    Raises RuntimeError if ffmpeg isn't installed or encode fails.
    """
    ffmpeg = _find_ffmpeg()
    if not ffmpeg:
        raise RuntimeError(
            "ffmpeg not found on PATH — install with `brew install ffmpeg`")
    proc = await asyncio.create_subprocess_exec(
        ffmpeg,
        "-hide_banner", "-loglevel", "error",
        "-f", "s16le", "-ar", str(sample_rate), "-ac", "1", "-i", "pipe:0",
        "-c:a", "libopus", "-b:a", f"{bitrate_kbps}k",
        "-application", "voip",      # tuned for voice, not music
        "-f", "ogg", "pipe:1",
        stdin=asyncio.subprocess.PIPE,
        stdout=asyncio.subprocess.PIPE,
        stderr=asyncio.subprocess.PIPE,
    )
    try:
        # 5 s ceiling — encode of ~30 s of speech finishes in <100 ms on
        # an M-series Mac; anything past 5 s is a sign ffmpeg hung. Also
        # guarantees we always reach the finally on outer cancellation so
        # the subprocess + its stdin pipe are cleaned up.
        out, err = await asyncio.wait_for(proc.communicate(input=pcm), timeout=5.0)
    except (asyncio.TimeoutError, asyncio.CancelledError):
        proc.kill()
        with contextlib.suppress(ProcessLookupError):
            await proc.wait()
        raise
    if proc.returncode != 0:
        raise RuntimeError(
            f"ffmpeg opus encode failed rc={proc.returncode}: {err.decode(errors='replace')[:200]}")
    return out


def _wrap_pcm_as_wav(pcm: bytes, sample_rate: int) -> bytes:
    """Wrap 16-bit LE mono PCM as a minimal WAV blob. Used when
    audio_format='wav' is requested explicitly (debug / no-ffmpeg fallback)."""
    buf = io.BytesIO()
    with wave.open(buf, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(sample_rate)
        w.writeframes(pcm)
    return buf.getvalue()


@dataclass
class FlashResult:
    text: str
    duration_ms: int
    utterances: list[dict]   # each: {start_time, end_time, text, words: [{text,start,end,confidence}]}

    @property
    def ok(self) -> bool:
        return bool(self.text)


class FlashError(RuntimeError):
    """Doubao server returned a non-success status code (e.g. 45000002 空音频)."""
    def __init__(self, code: str, message: str, payload: dict | None = None):
        super().__init__(f"flash asr error code={code} msg={message}")
        self.code = code
        self.message = message
        self.payload = payload


async def transcribe(
    pcm_int16le_mono: bytes,
    *,
    app_key: str,
    access_key: str,
    sample_rate: int = 16000,
    uid: str = "stackchan",
    enable_punc: bool = True,
    enable_itn: bool = True,
    enable_ddc: bool = True,
    audio_format: str = "ogg_opus",   # "ogg_opus" (default, fast upload) | "wav"
    opus_bitrate_kbps: int = 16,
    timeout_s: float = 8.0,
    max_retries: int = 1,
    http_client: httpx.AsyncClient | None = None,
) -> FlashResult:
    """One-shot ASR. Blocks until the server returns the full transcript.

    Args:
        pcm_int16le_mono: raw audio bytes — 16-bit signed LE little-endian
            mono samples at `sample_rate` Hz.
        app_key: legacy console APP_KEY (also called appid).
        access_key: legacy console ACCESS_TOKEN.
        sample_rate: 16000 is what mic_pcm uses; 8000/16000/24000 also valid.
        enable_punc / itn / ddc: see Doubao docs §request fields.
        audio_format: "ogg_opus" (default) shrinks 5 s of audio from 160 KB
            WAV → ~10 KB OPUS, which is the difference between "uploads in
            ~2 s" and "uploads in 30 s + times out" on bandwidth-shaped
            links. "wav" bypasses ffmpeg (debug / known-good baseline).
        opus_bitrate_kbps: 16 kbps is fine for speech; 32 kbps trades upload
            time for marginal accuracy improvements in noisy audio.
        timeout_s: hard cap on each HTTP attempt.
        max_retries: retry on timeout/network error/5xx.
        http_client: optional pre-created AsyncClient for connection reuse.

    Returns:
        FlashResult with `.text` (possibly empty if pure silence — that is
        NOT an error, just an empty transcript).

    Raises:
        FlashError on a non-success status code from Doubao.
        httpx.RequestError on unrecoverable network failures.
        RuntimeError if audio_format='ogg_opus' but ffmpeg is unavailable.
    """
    if audio_format == "ogg_opus":
        audio_bytes = await _encode_pcm_to_opus(pcm_int16le_mono, sample_rate,
                                                 bitrate_kbps=opus_bitrate_kbps)
        wire_fmt = "ogg_opus"
    elif audio_format == "wav":
        audio_bytes = _wrap_pcm_as_wav(pcm_int16le_mono, sample_rate)
        wire_fmt = "wav"
    else:
        raise ValueError(f"unsupported audio_format={audio_format!r}; use 'ogg_opus' or 'wav'")

    body = {
        "user":    {"uid": uid},
        "audio":   {"data": base64.b64encode(audio_bytes).decode(), "format": wire_fmt},
        "request": {
            "model_name":  "bigmodel",
            "enable_punc": enable_punc,
            "enable_itn":  enable_itn,
            "enable_ddc":  enable_ddc,
        },
    }
    headers = {
        "X-Api-App-Key":     app_key,
        "X-Api-Access-Key":  access_key,
        "X-Api-Resource-Id": RESOURCE_ID,
        "X-Api-Request-Id":  str(uuid.uuid4()),
        "X-Api-Sequence":    "-1",   # 固定值 per docs
        "Content-Type":      "application/json",
    }

    log.debug("flash asr posting: pcm=%d B audio=%d B (%s) json=%d B",
              len(pcm_int16le_mono), len(audio_bytes), wire_fmt, len(body))

    own_client = http_client is None
    client = http_client or httpx.AsyncClient(timeout=timeout_s)
    last_exc: BaseException | None = None
    r: httpx.Response | None = None
    try:
        # Doubao flash latency is bimodal: usually <2 s, sometimes >15 s when
        # the worker pool is cold or the upload is bandwidth-throttled. Up
        # to max_retries retries with linear backoff covers the slow case
        # without making typical calls slow.
        for attempt in range(max_retries + 1):
            try:
                r = await client.post(URL, headers=headers, json=body, timeout=timeout_s)
                if r.status_code >= 500 and attempt < max_retries:
                    log.warning("flash asr attempt %d/%d got http=%d, retrying",
                                attempt + 1, max_retries + 1, r.status_code)
                    await asyncio.sleep(1.0 * (attempt + 1))
                    continue
                break
            except (httpx.TimeoutException, httpx.NetworkError) as e:
                last_exc = e
                log.warning("flash asr attempt %d/%d failed: %r",
                            attempt + 1, max_retries + 1, e)
                if attempt < max_retries:
                    await asyncio.sleep(1.0 * (attempt + 1))
        if r is None:
            assert last_exc is not None
            raise last_exc
    finally:
        if own_client:
            await client.aclose()

    status_code = r.headers.get("X-Api-Status-Code") or ""
    status_msg  = r.headers.get("X-Api-Message")     or ""
    logid       = r.headers.get("X-Tt-Logid")        or ""
    log.info("flash asr: http=%d api_code=%s msg=%s logid=%s",
             r.status_code, status_code, status_msg, logid)

    try:
        j = r.json()
    except json.JSONDecodeError as e:
        raise FlashError("BAD_JSON", str(e), {"raw": r.text[:500]}) from e

    if status_code and status_code != "20000000":
        raise FlashError(status_code, status_msg or r.text[:200], j)

    result = j.get("result") or {}
    audio_info = j.get("audio_info") or {}
    text = (result.get("text") or "").strip()
    duration_ms = int(audio_info.get("duration") or 0)
    utterances = result.get("utterances") or []
    return FlashResult(text=text, duration_ms=duration_ms, utterances=utterances)

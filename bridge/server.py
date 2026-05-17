"""
热狗 bridge server — device <-> Hermes.

Replaces stackproxy. Speaks ocsc.v2 to the device over WebSocket (:8765) and
exposes device capabilities to Hermes over MCP (:8766, added in P2c). For now
(P2a skeleton) it provides:
  - ocsc.v2 WS server: hello handshake, reconnect protocol, RPC id routing, events
  - admin HTTP (:8766): GET /healthz, GET /status, POST /rpc  (test harness)

Audio (Doubao ASR/TTS), camera/display media handling, and the MCP server land
in later phases (P3/P4/P2c). The WS-core (Device/Hub/ws_handler/handle_text) is
ported from stackproxy/server.py — ocsc.v1 and v2 frame shapes are identical.

Run:  python3 server.py   (typically on the same host as the agent)
"""
from __future__ import annotations

import argparse
import asyncio
import contextlib
import logging
import time
import uuid
from dataclasses import dataclass, field
from typing import Any

import websockets
from aiohttp import web

from protocol import (
    decode_text, decode_binary, encode_binary, frame_req, frame_err, frame_pong,
    frame_hello_ack, frame_tts_start, frame_tts_end,
    KIND_MIC_PCM, KIND_TTS_PCM, KIND_CAM_JPEG, KIND_DISP_IMG, KIND_DISP_RGB565,
)
import struct
import aiohttp
import config as bridge_config
import doubao_asr_flash
import doubao_tts_unidirectional
import doubao_tts
import doubao_asr
import tracker as face_tracker
import display as screen

# ----- SenseVoice local ASR ---------------------------------------------
# Replaces Doubao's streaming ASR. SenseVoice is a local funasr-based
# model running on the openclaw mini (sensevoice-server/server.py). One
# HTTP POST per utterance, ~0.5-2 s CPU latency. No streaming, no session
# timeouts, no protocol surprises. Audio quality matters more (no AGC),
# but our notch+HP filter cleans things up enough.
SENSEVOICE_URL = "http://127.0.0.1:18095/transcribe"


async def sensevoice_transcribe(pcm16le: bytes,
                                  sample_rate: int = 16000) -> str:
    """POST a WAV of PCM to the local SenseVoice server, return transcript."""
    if not pcm16le:
        return ""
    # Wrap raw PCM in a minimal WAV header.
    n_bytes = len(pcm16le)
    wav = (b"RIFF"
           + struct.pack("<I", 36 + n_bytes)
           + b"WAVE"
           + b"fmt "
           + struct.pack("<IHHIIHH", 16, 1, 1, sample_rate,
                          sample_rate * 2, 2, 16)
           + b"data"
           + struct.pack("<I", n_bytes)
           + pcm16le)
    form = aiohttp.FormData()
    form.add_field("file", wav, filename="utt.wav",
                   content_type="audio/wav")
    async with aiohttp.ClientSession() as s:
        try:
            async with s.post(SENSEVOICE_URL, data=form,
                                timeout=aiohttp.ClientTimeout(total=20)) as r:
                data = await r.json()
                if not r.ok:
                    log.warning("[asr] sensevoice %d: %s", r.status, data)
                    return ""
                return (data.get("text") or "").strip()
        except Exception:
            log.exception("[asr] sensevoice POST failed")
            return ""


# ----- Doubao oneshot ASR (Seed-ASR 2.0 streaming, fed as one shot) -----
# Hand the whole utterance to Seed-ASR in chunked feed() + is_last, then
# wait_finish. Streaming protocol is happy with this — returns one final
# utterance and closes. Trades a bit of latency (~300-500 ms) for
# accuracy and the absence of SenseVoice's stray-period hallucinations.
async def doubao_transcribe(pcm16le: bytes,
                            sample_rate: int = 16000,
                            uid: str = "bridge") -> str:
    if not pcm16le:
        return ""
    appid, token = doubao_tts._load_creds()
    finals: list[str] = []
    partials: list[str] = []

    async def on_partial(text, _utts):
        partials.append(text or "")

    async def on_final(text, _utts):
        if text:
            finals.append(text)

    sess = doubao_asr.DoubaoAsrSession(
        app_key=appid, access_key=token,
        on_partial=on_partial, on_final=on_final, uid=uid,
    )
    try:
        await sess.start()
        # Feed in ~100 ms chunks so the server-side packetiser stays happy
        # (it expects roughly streaming-paced input; one giant frame can hang).
        step = sample_rate * 2 // 10  # 100 ms @ 16k mono = 3200 bytes
        for off in range(0, len(pcm16le), step):
            await sess.feed(pcm16le[off:off + step], is_last=False)
        await sess.feed(b"", is_last=True)
        text = await sess.wait_finish(timeout=10.0)
    except Exception:
        log.exception("[asr] doubao oneshot failed")
        return ""
    if not text:
        text = finals[-1] if finals else (partials[-1] if partials else "")
    return (text or "").strip()


# ----- mic noise filtering ---------------------------------------------
# The CoreS3 chassis-mounted mic picks up a strong 317 Hz tone (almost
# certainly LCD backlight PWM or servo-motor hum coupling into the mic
# circuit). Visible as ~3 dB above everything else in FFT analysis of
# silent and speech captures alike. Without filtering, Doubao ASR
# mistranscribes ("今天天气怎么样" → "湖南镇" / "等等等等等等").
#
# A narrow IIR notch (Q=15) at 317 Hz wipes the tone with ~0.2 dB
# collateral damage to nearby speech harmonics. State is kept per-device
# so filter memory persists across mic_pcm frames.

import numpy as np
from scipy.signal import iirnotch, butter, lfilter, lfilter_zi


def _make_mic_filter():
    """Cascade two IIR sections, each with its own delay state:
      1) high-pass at 80 Hz, 4th-order Butterworth — kills 36-85 Hz rumble
      2) notch at 317 Hz, Q=15 — kills the chassis hum tone
    """
    b_hp, a_hp = butter(4, 80.0, btype="high", fs=16000)
    zi_hp = lfilter_zi(b_hp, a_hp)
    b_notch, a_notch = iirnotch(317.0, Q=15.0, fs=16000)
    zi_notch = lfilter_zi(b_notch, a_notch)
    return {"hp": (b_hp, a_hp, zi_hp), "notch": (b_notch, a_notch, zi_notch),
            "primed": False}


def _apply_mic_filter(state, pcm_bytes: bytes) -> bytes:
    samples = np.frombuffer(pcm_bytes, dtype="<i2").astype(np.float32)
    if len(samples) == 0:
        return pcm_bytes
    if not state["primed"]:
        b_hp, a_hp, zi_hp = state["hp"]
        b_n,  a_n,  zi_n  = state["notch"]
        state["hp"]    = (b_hp, a_hp, zi_hp * samples[0])
        state["notch"] = (b_n,  a_n,  zi_n  * samples[0])
        state["primed"] = True
    b_hp, a_hp, zi_hp = state["hp"]
    y, zi_hp = lfilter(b_hp, a_hp, samples, zi=zi_hp)
    state["hp"] = (b_hp, a_hp, zi_hp)
    b_n, a_n, zi_n = state["notch"]
    y, zi_n = lfilter(b_n, a_n, y, zi=zi_n)
    state["notch"] = (b_n, a_n, zi_n)
    return np.clip(y, -32768, 32767).astype("<i2").tobytes()

log = logging.getLogger("bridge")


# =========================================================================
# Device / Hub
# =========================================================================

@dataclass
class Device:
    device_id: str
    boot_count: int
    connection_id: str
    fw: str
    caps: list[str]
    ws: Any
    remote_addr: str
    pending: dict[str, asyncio.Future] = field(default_factory=dict)
    sid_seq: int = 1
    last_seen: float = field(default_factory=time.time)
    boot_started_at: float = field(default_factory=time.time)
    asr: Any = None                                  # active doubao_asr.DoubaoAsrSession, if any
    asr_sid: int = 0
    mic_dump: bytearray | None = None                # if set, mic_pcm frames also append here
    pending_jpegs: dict[int, asyncio.Future] = field(default_factory=dict)
    tracker: Any = None                              # face_tracker.Tracker, if running
    stream_task: Any = None                          # asyncio.Task of currently-playing stream_pcm, if any
    # Serialise every TTS stream to this device. Multiple call sites can
    # request playback (voice_loop, HTTP /say, hotdog skill `sc say`); the
    # bug we saw was a 47 s 《将进酒》 stream and a 2.5 s "I can't answer"
    # stream interleaving on the wire, then mic opening on top of a still-
    # playing speaker → ASR self-feedback. Hold this lock around every
    # stream_pcm call so they queue cleanly.
    tts_lock: asyncio.Lock = field(default_factory=asyncio.Lock)
    voice_task: Any = None                           # asyncio.Task of currently-running voice_loop, if any
    mic_prebuf: list[bytes] | None = None            # buffer mic_pcm frames while ASR is connecting
    mic_filter: dict = field(default_factory=_make_mic_filter)  # 317 Hz notch state
    tts_done_evt: asyncio.Event = field(default_factory=asyncio.Event)
    # Rolling dialog history fed back to Doubao ASR as `context_type=dialog_ctx`
    # so consecutive turns benefit from earlier-turn vocabulary / names /
    # references. Ordered newest-first (per Volcengine spec). Capped to
    # DIALOG_HISTORY_MAX entries.
    dialog_history: list[str] = field(default_factory=list)
    # Outbound buffer that aggregates mic_pcm frames before they're forwarded
    # to Doubao — see ASR_FRAME_BYTES note in handle_binary.
    asr_outbuf: bytearray = field(default_factory=bytearray)
    tts_active: bool = False                          # speaker is actually playing or has queued PCM

    def next_sid(self) -> int:
        sid = self.sid_seq
        self.sid_seq = (self.sid_seq + 1) & 0xFFFF or 1
        return sid


class Hub:
    def __init__(self) -> None:
        self.devices: dict[str, Device] = {}
        self._lock = asyncio.Lock()

    async def register(self, device: Device) -> None:
        async with self._lock:
            existing = self.devices.get(device.device_id)
            if existing is not None and existing.ws is not device.ws:
                if device.boot_count >= existing.boot_count:
                    log.info("[hub] replacing %s (old boot=%d new=%d)",
                             device.device_id, existing.boot_count, device.boot_count)
                    with contextlib.suppress(Exception):
                        await existing.ws.close(code=4000, reason="superseded")
                else:
                    log.warning("[hub] stale hello from %s boot=%d (current=%d)",
                                device.device_id, device.boot_count, existing.boot_count)
            self.devices[device.device_id] = device

    async def unregister(self, device: Device) -> None:
        async with self._lock:
            if self.devices.get(device.device_id) is device:
                self.devices.pop(device.device_id, None)
                log.info("[hub] unregistered %s", device.device_id)

    def get(self, device_id: str | None) -> Device | None:
        if not self.devices:
            return None
        if device_id and device_id != "default":
            return self.devices.get(device_id)
        return next(iter(self.devices.values()))

    async def rpc(self, device: Device, method: str,
                  params: dict | None = None, timeout: float = 6.0) -> dict:
        rpc_id = uuid.uuid4().hex[:12]
        fut: asyncio.Future = asyncio.get_event_loop().create_future()
        device.pending[rpc_id] = fut
        try:
            await device.ws.send(frame_req(rpc_id, method, params))
            return await asyncio.wait_for(fut, timeout=timeout)
        finally:
            device.pending.pop(rpc_id, None)


# =========================================================================
# WS handler
# =========================================================================

async def ws_handler(ws, hub: Hub) -> None:
    device: Device | None = None
    try:
        try:
            first = await asyncio.wait_for(ws.recv(), timeout=10.0)
        except asyncio.TimeoutError:
            log.warning("[ws] hello timeout from %s", ws.remote_address)
            return
        msg = decode_text(first) if isinstance(first, str) else None
        if msg is None or msg.get("t") != "hello":
            with contextlib.suppress(Exception):
                await ws.send(frame_err("0", "bad_hello", "first frame must be hello"))
            return

        remote = getattr(ws, "remote_address", ("?", 0))
        device = Device(
            device_id=msg.get("device_id") or f"anon-{uuid.uuid4().hex[:6]}",
            boot_count=int(msg.get("boot_count", 0)),
            connection_id=msg.get("connection_id") or uuid.uuid4().hex,
            fw=msg.get("fw", "?"),
            caps=msg.get("caps", []),
            ws=ws,
            remote_addr=f"{remote[0]}:{remote[1]}",
        )
        await hub.register(device)
        await ws.send(frame_hello_ack(ts=int(time.time() * 1000)))
        log.info("[ws] hello %s boot=%d fw=%s caps=%s remote=%s",
                 device.device_id, device.boot_count, device.fw,
                 device.caps, device.remote_addr)
        # Push the default idle face so the screen comes alive immediately
        # after every (re)connect. Fire-and-forget so it doesn't block the
        # ws main loop.
        asyncio.create_task(_push_idle_face(device))
        # Re-arm WakeNet on every reconnect — covers the case where the
        # last voice loop got interrupted (bridge restart) and left
        # wakeword's `s_active` stuck at false.
        async def _rearm_wake() -> None:
            await asyncio.sleep(0.6)
            with contextlib.suppress(Exception):
                await hub.rpc(device, "wake_resume", {}, timeout=2.0)
        asyncio.create_task(_rearm_wake())

        async for raw in ws:
            device.last_seen = time.time()
            if isinstance(raw, bytes):
                await handle_binary(hub, device, raw)
            else:
                await handle_text(hub, device, raw)
    except websockets.ConnectionClosed:
        pass
    finally:
        if device is not None:
            await hub.unregister(device)


async def handle_text(hub: Hub, device: Device, raw: str) -> None:
    msg = decode_text(raw)
    if msg is None:
        log.warning("[ws] bad text from %s: %r", device.device_id, raw[:80])
        return
    t = msg.get("t")

    if t == "ping":
        await device.ws.send(frame_pong(msg.get("ts", int(time.time() * 1000))))
    elif t in ("res", "err"):
        fut = device.pending.get(msg.get("id", ""))
        if fut and not fut.done():
            if t == "res":
                fut.set_result(msg.get("d") or {})
            else:
                fut.set_exception(RuntimeError(
                    f"{msg.get('code', 'err')}: {msg.get('msg', '')}"))
    elif t == "evt":
        name = msg.get("name")
        if name == "wake":
            word = (msg.get("d") or {}).get("word", "?")
            log.info("[evt] %s WAKE word=%s", device.device_id, word)
            # If a previous voice loop is mid-execution AND audio is genuinely
            # playing right now (not just queued-and-already-drained), this
            # wake interrupts it. We rely on `tts_active` rather than just
            # "stream_task not done" because the user's most common false
            # alarm is WakeNet triggering on the *speaker's own audio* —
            # cancelling at that point cuts off the last syllables of every
            # reply. Once tts.done fires from the device, the speaker is
            # idle for real and a wake here is a genuine new turn.
            already_busy = (device.voice_task is not None
                            and not device.voice_task.done())
            if already_busy and device.tts_active:
                log.info("[voice] WAKE interrupts active TTS")
                if device.stream_task and not device.stream_task.done():
                    device.stream_task.cancel()
                device.voice_task.cancel()
                asyncio.create_task(_safe_tts_stop(hub, device))
            elif already_busy:
                # Loop is running but no audio playing — likely capture/
                # think phase. Cancel and restart so the user can re-prompt.
                device.voice_task.cancel()
            device.voice_task = asyncio.create_task(voice_loop(hub, device))
        elif name == "tts.done":
            device.tts_active = False
            device.tts_done_evt.set()
            log.info("[evt] %s tts.done", device.device_id)
        else:
            log.info("[evt] %s %s %s", device.device_id, name, msg.get("d"))
    elif t in ("mic.start", "mic.end"):
        log.info("[%s] %s sid=%s (audio wired in P3)", device.device_id, t, msg.get("sid"))
    else:
        log.debug("[ws] unhandled text %s from %s", t, device.device_id)


async def handle_binary(hub: Hub, device: Device, data: bytes) -> None:
    frame = decode_binary(data)
    if frame is None:
        return
    if frame.kind == KIND_MIC_PCM:
        # Apply notch (317 Hz hum) + HP (80 Hz rumble) then:
        #   • full-rate copy → mic_dump (RMS silence detector wants every
        #     sample)
        #   • aggregate ≥120 ms → Doubao (per Volcengine docs the recommended
        #     packet size is 100-200 ms; sending 40 ms packets at 25 fps
        #     triggers 45000081 "Timeout waiting next packet" because the
        #     server-side pacer expects ~one packet per ~150 ms)
        # Bridge mic IIR filter was destroying Doubao's recognition (offline
        # A/B test: raw dump -> '现在几点钟了？', filtered dump -> ''). The
        # 80 Hz HP + 317 Hz notch cascade with the lazy zi-priming injects a
        # large transient at every frame boundary; the cumulative effect
        # destroys voiced-band content the ASR needs. Use the raw payload
        # directly. If chassis hum becomes a problem we'll add a wider-band
        # cleaner that's audibly transparent (or do it on-device).
        clean = bytes(frame.payload)
        if device.mic_dump is not None:
            device.mic_dump.extend(clean)
            if (frame.seq & 0xf) == 0:
                log.info("[bin] mic_pcm seq=%u acc=%d B",
                         frame.seq, len(device.mic_dump))
        # No live ASR streaming any more — voice_loop captures into
        # device.mic_dump and POSTs the whole utterance to flash ASR.
    elif frame.kind == KIND_CAM_JPEG:
        fut = device.pending_jpegs.pop(frame.sid, None)
        if fut is not None and not fut.done():
            fut.set_result(bytes(frame.payload))
            log.info("[bin] cam_jpeg sid=%d %d bytes -> waiter", frame.sid, len(frame.payload))
        else:
            log.info("[bin] cam_jpeg sid=%d %d bytes (no waiter)",
                     frame.sid, len(frame.payload))
    else:
        log.debug("[bin] kind=0x%02x %d bytes", frame.kind, len(frame.payload))


# =========================================================================
# admin HTTP — test harness until the MCP server lands (P2c)
# =========================================================================

async def http_healthz(_req: web.Request) -> web.Response:
    return web.json_response({"ok": True})


async def http_status(req: web.Request) -> web.Response:
    hub: Hub = req.app["hub"]
    return web.json_response({
        "ok": True,
        "ts": int(time.time() * 1000),
        "devices": [
            {
                "device_id": d.device_id,
                "boot_count": d.boot_count,
                "fw": d.fw,
                "remote": d.remote_addr,
                "caps": d.caps,
                "last_seen_age_s": round(time.time() - d.last_seen, 1),
                "boot_uptime_s": round(time.time() - d.boot_started_at, 1),
            }
            for d in hub.devices.values()
        ],
    })


async def stream_pcm(device: Device, pcm: bytes, sample_rate: int = 16000) -> int:
    """Send `pcm` to `device` as KIND_TTS_PCM frames at exact realtime pace.

    Architecture (mirroring xiaozhi-esp32, the canonical streaming-audio
    firmware): bridge ships chunks at an **absolute-time schedule** so the
    cumulative slip from `ws.send` overhead can't accumulate. Device side
    blocks on a fat PSRAM-backed queue (no drops) — backpressure flows
    naturally from I2S drain rate back through TCP to this loop.

    Why this matters: my old loop did `await ws.send(); await sleep(60ms)`.
    Each iteration actually took 65-70 ms (ws.send isn't free). After ~333
    chunks (≈ 20 s of audio) the cumulative slip ate the entire device
    cushion → glitches in the back half. Absolute-time scheduling fixes
    that — every chunk N is sent at t0 + N*60ms regardless of how long
    the previous send took. """
    # Serialise every TTS stream to this device — if a long playback (e.g.
    # `sc say "将进酒…"`) is in flight when voice_loop also wants to push
    # a reply, the second caller blocks until the first finishes. Without
    # this two streams interleave on the wire and the device only really
    # finishes the FIRST when the SECOND has already started; we'd open
    # mic on top of still-playing audio and ASR ate its own echo.
    async with device.tts_lock:
        return await _stream_pcm_locked(device, pcm, sample_rate)


async def _stream_pcm_locked(device: Device, pcm: bytes,
                              sample_rate: int) -> int:
    sid = device.next_sid()
    est_ms = (len(pcm) // 2) * 1000 // sample_rate
    device.tts_done_evt.clear()
    device.tts_active = True
    await device.ws.send(frame_tts_start(sid=sid, sr=sample_rate, est_ms=est_ms))

    CHUNK = 1920                                 # 60 ms @ 16k mono int16
    chunk_s = CHUNK / 2 / sample_rate            # 0.060 at 16 kHz

    # First N chunks are pre-buffer — fired immediately so the device speaker
    # queue starts with cushion. After that, each chunk N is targeted at
    # t0 + (N - PRIME) * chunk_s wall time at exactly 1×realtime.
    #
    # Earlier iteration used PACING=0.85 (15 % faster than realtime) to grow
    # cushion. That broke things: piling frames into the device's PSRAM
    # queue starved the ws keepalive task on the device, the bridge's 20 s
    # ping timeout fired, the WS got force-closed mid-utterance. PRIME=16
    # × 60 ms ≈ 1 s prebuffer is plenty without overloading.
    PRIME = 16
    loop = asyncio.get_event_loop()
    t0 = loop.time()
    seq = 0
    sent = 0
    for off in range(0, len(pcm), CHUNK):
        if seq >= PRIME:
            target = t0 + (seq - PRIME + 1) * chunk_s
            now = loop.time()
            if target > now:
                await asyncio.sleep(target - now)
        await device.ws.send(encode_binary(KIND_TTS_PCM, sid, seq, pcm[off:off + CHUNK]))
        seq += 1
        sent += min(CHUNK, len(pcm) - off)
    await device.ws.send(frame_tts_end(sid=sid))
    elapsed_ms = (loop.time() - t0) * 1000
    # IMPORTANT: stream_pcm now owns "wait until audio is really played".
    # The bridge used to rely on device's `tts.done` event but that arrives
    # 1-10 s late inconsistently (event reporter is low-priority on device).
    # If we open mic before audio finishes the speaker bleeds into the mic
    # and ASR catches its own echo as garbage transcripts. So: sleep until
    # the wall-clock time matches the audio's true duration + 0.4 s margin.
    audio_done_at = t0 + (est_ms / 1000.0) + 0.4
    now = loop.time()
    if audio_done_at > now:
        await asyncio.sleep(audio_done_at - now)
    log.info("stream_pcm sid=%d %d B in %.0fms (audio=%dms, drift=%+dms, waited=%dms)",
             sid, sent, elapsed_ms, est_ms, int(elapsed_ms - est_ms),
             int((loop.time() - t0) * 1000 - elapsed_ms))
    return sent


# -------------------------------------------------------------------------
# Streaming TTS variant: accept an async iterator of PCM chunks and pipe
# them onto the device as soon as they land, instead of buffering the
# whole reply. The whole point of the Doubao unidirectional endpoint is
# TTFB: first audio chunk lands at ~400-800 ms; if we wait for the entire
# synth to finish before opening stream_pcm, we waste that advantage
# (5 s reply → user hears nothing until ~1.5 s after we'd otherwise have
# started playing).
# -------------------------------------------------------------------------


class _StreamEnd:
    """Sentinel marking end-of-stream from producer."""
    __slots__ = ()


class _StreamErr:
    """Sentinel carrying an exception from producer back to consumer."""
    __slots__ = ("exc",)
    def __init__(self, exc: BaseException) -> None:
        self.exc = exc


_STREAM_END = _StreamEnd()


async def stream_pcm_async(device: Device,
                            chunks: "AsyncIterator[bytes]",
                            sample_rate: int = 16000) -> int:
    """Stream PCM chunks to `device` in real time.

    Producer (the supplied async iterator) is consumed in a background task
    that re-chunks arbitrary-sized inputs into 1920-byte (60 ms) frames and
    pushes them onto a bounded queue. The consumer ships them on the
    absolute-time schedule (same pacing as `stream_pcm`), so the device
    cushion still grows from a PRIME prebuffer but **no producer-side
    buffering** holds up the first frame.
    """
    async with device.tts_lock:
        return await _stream_pcm_async_locked(device, chunks, sample_rate)


async def _stream_pcm_async_locked(device: Device,
                                    chunks: "AsyncIterator[bytes]",
                                    sample_rate: int) -> int:
    sid = device.next_sid()
    device.tts_done_evt.clear()
    device.tts_active = True
    # est_ms unknown until producer finishes; device tolerates est_ms=0
    # (it's only used for the UI progress hint).
    await device.ws.send(frame_tts_start(sid=sid, sr=sample_rate, est_ms=0))

    CHUNK = 1920                                 # 60 ms @ 16k mono int16
    chunk_s = CHUNK / 2 / sample_rate
    PRIME = 16
    sent_end = False

    queue: "asyncio.Queue[bytes | _StreamEnd | _StreamErr]" = asyncio.Queue(maxsize=256)

    async def producer() -> None:
        buf = bytearray()
        try:
            async for c in chunks:
                if not c:
                    continue
                buf.extend(c)
                # Re-chunk into 60-ms frames. The Doubao API hands us
                # arbitrary-sized base64 frames (typically 6-8 KB each =
                # 187-250 ms), so one input usually fans out to 3-4 output
                # frames.
                while len(buf) >= CHUNK:
                    await queue.put(bytes(buf[:CHUNK]))
                    del buf[:CHUNK]
            if buf:
                await queue.put(bytes(buf))      # tail (< CHUNK)
            await queue.put(_STREAM_END)
        except BaseException as e:               # noqa: BLE001 — propagate any
            # Use put_nowait + suppress: if the consumer has already exited
            # (e.g. voice_loop cancelled by a new wake word), the queue may
            # be full AND no one is reading it, so a blocking `await put`
            # would deadlock the producer forever.
            with contextlib.suppress(Exception):
                queue.put_nowait(_StreamErr(e))

    prod_task = asyncio.create_task(producer())

    loop = asyncio.get_event_loop()
    t0: float | None = None
    seq = 0
    sent = 0
    first_chunk_ms = -1

    try:
        while True:
            item = await queue.get()
            if isinstance(item, _StreamEnd):
                break
            if isinstance(item, _StreamErr):
                raise item.exc
            chunk_bytes = item
            if t0 is None:
                t0 = loop.time()
                # Real first-chunk wall time would be measured before we
                # arm t0 (it'd require capturing the moment we *started
                # waiting* for the first item). We deliberately reset the
                # clock here so the pacing math after PRIME is anchored to
                # when the device starts playing, not when the HTTP POST
                # opened. The TTFB is logged by doubao_tts_unidirectional.
                first_chunk_ms = 0
            if seq >= PRIME:
                target = t0 + (seq - PRIME + 1) * chunk_s
                now = loop.time()
                if target > now:
                    await asyncio.sleep(target - now)
            await device.ws.send(encode_binary(KIND_TTS_PCM, sid, seq, chunk_bytes))
            seq += 1
            sent += len(chunk_bytes)
    finally:
        # Producer normally completes naturally before we exit; cancel only
        # if we bailed early.
        if not prod_task.done():
            prod_task.cancel()
            with contextlib.suppress(asyncio.CancelledError, Exception):
                await prod_task
        # Always send tts_end so the device exits its TTS receive state
        # cleanly, even on consumer-side exception. Also always clear
        # tts_active so wake-handler doesn't see a stale "still talking".
        if not sent_end:
            with contextlib.suppress(Exception):
                await device.ws.send(frame_tts_end(sid=sid))
            sent_end = True
        device.tts_active = False

    if t0 is None:
        # Producer emitted zero bytes — nothing to wait for.
        log.warning("stream_pcm_async sid=%d empty stream", sid)
        return 0

    elapsed_ms = int((loop.time() - t0) * 1000)
    audio_ms = (sent // 2) * 1000 // sample_rate
    # Same trailing wait as stream_pcm: hold the lock until speaker truly
    # drained, so the next voice turn doesn't open mic while audio plays.
    audio_done_at = t0 + audio_ms / 1000.0 + 0.4
    now = loop.time()
    if audio_done_at > now:
        await asyncio.sleep(audio_done_at - now)
    log.info("stream_pcm_async sid=%d %d B in %dms (audio=%dms, first=%dms)",
             sid, sent, elapsed_ms, audio_ms, first_chunk_ms)
    return sent


async def _ship_jpeg(device: Device, jpeg: bytes) -> None:
    sid = device.next_sid()
    await device.ws.send(encode_binary(KIND_DISP_IMG, sid, 0, jpeg))


async def _ship_rgb565(device: Device, rgb: bytes) -> None:
    """Push a 320×240 RGB565 frame. ~153 KB, takes ~50 ms over WiFi to land
    and ~30 ms to blit on the device. Used wherever we render text — JPEG's
    chroma subsampling otherwise blurs Chinese glyph strokes visibly."""
    sid = device.next_sid()
    await device.ws.send(encode_binary(KIND_DISP_RGB565, sid, 0, rgb))


async def _ship_text(device: Device, text: str, *, title: str = "",
                     face: str | None = None, bg=None, fg=None) -> None:
    """Convenience: render text via PIL, push as RGB565 (crisp glyphs)."""
    bg = bg or screen.BG_DEFAULT
    fg = fg or screen.FG_DEFAULT
    img = screen.render_text(text, title=title, face=face, bg=bg, fg=fg)
    await _ship_rgb565(device, screen.to_rgb565(img))


async def _safe_tts_stop(hub: Hub, device: Device) -> None:
    """RPC `tts_stop` (drains device-side play queue). Swallows errors."""
    with contextlib.suppress(Exception):
        await hub.rpc(device, "tts_stop", {}, timeout=2.0)


async def _set_led(hub: Hub, device: Device, r: int, g: int, b: int) -> None:
    """Fire-and-forget LED ring color. Errors logged but not raised."""
    try:
        await hub.rpc(device, "set_led", {"r": r, "g": g, "b": b}, timeout=2.0)
    except Exception:
        log.debug("[led] set_led failed (non-fatal)")


async def _push_idle_face(device: Device) -> None:
    """Show the default idle face once the device's link is up."""
    # tiny grace period so the device has flushed `hello.ack` first
    await asyncio.sleep(0.4)
    with contextlib.suppress(Exception):
        await _ship_jpeg(device, screen.face_jpeg("idle"))
        log.info("[disp] idle face -> %s", device.device_id)


# =========================================================================
# voice loop — wake word → ASR → Hermes → TTS → re-arm wake
# =========================================================================

# Tunables. We don't terminate on Doubao's first "definite" event because
# that fires after every short pause — totally unreliable for mid-sentence
# breath pauses. Instead we watch the partial-arrival stream: if no new
# partial arrives for VOICE_SILENCE_S (and at least one partial has been
# seen), the user has stopped speaking and we finalize.
VOICE_MAX_RECORD_S = 15.0      # hard ceiling
VOICE_SILENCE_S    = 1.8       # seconds without new partial → finalize
ASR_END_WINDOW_MS  = 2000      # Doubao's own server-side detector (we mostly ignore its result)
HERMES_TIMEOUT_S   = 35.0
HERMES_SKILLS      = "hotdog"
# Per-device dialog history cap fed back to Doubao as `context`. Volcengine
# limit is ~800 tokens / 20 turns; 24 entries (≈12 round trips) is well inside
# and matches the rule-of-thumb "remember the last few minutes of chat".
DIALOG_HISTORY_MAX = 24
# Aggregate mic_pcm frames (device sends 40 ms each) into 120 ms chunks before
# pushing to Doubao — the docs warn that <100 ms packets at >5/s trigger the
# server pacer's "Timeout waiting next packet" error (45000081). 120 ms / 3840 B
# at 16 kHz mono.
ASR_FRAME_BYTES = 6400   # 200 ms @ 16k mono — Volcengine 双向流式 optimal
# Max times we'll re-listen within one voice loop after an empty ASR result
# (each failure is shown on screen). After this many strikes we give up.
ASR_RETRY_LIMIT = 2

# LED ring colors per voice-loop phase. Pure-color cues are enough to convey
# state without a breathing animation (device's set_led is solid-on only).
LED_IDLE      = (0, 0, 0)
LED_LISTENING = (0, 80, 180)   # cool blue → "I'm listening"
LED_THINKING  = (140, 60, 180) # purple → "I'm working on it"
LED_TALKING   = (50, 180, 90)  # green → "I'm speaking"
LED_ERROR     = (180, 50, 20)  # red → "nothing heard"


async def voice_loop(hub: Hub, device: Device) -> None:
    """One iteration of: prompt → ASR → Hermes → TTS → idle. Triggered by a
    wake event from the device. Auto-resumes WakeNet at the end. Cancellable —
    a second wake-word during the loop will cancel and start a fresh one."""
    t0 = time.time()
    log.info("[voice] %s loop start", device.device_id)
    cancelled = False
    try:
        turn = 0
        asr_strikes = 0     # consecutive empty-ASR results; resets on success
        had_success = False # any successful turn → silence on next turn = exit
        while True:
            turn += 1
            is_followup = had_success     # follow-up only after a real reply
            is_retry = asr_strikes > 0    # we're re-listening after empty ASR
            max_record = 5.0 if is_followup else VOICE_MAX_RECORD_S

            # Phase 1: LISTENING screen + LED
            asyncio.create_task(_set_led(hub, device, *LED_LISTENING))
            if is_retry:
                with contextlib.suppress(Exception):
                    await hub.rpc(device, "show_text",
                                  {"title": "请再说一次",
                                   "text": f"刚才没听清（第 {asr_strikes} 次）"},
                                  timeout=2.0)
            elif is_followup:
                with contextlib.suppress(Exception):
                    await hub.rpc(device, "show_text",
                                  {"title": "继续吗？",
                                   "text": "请讲，没说话就结束…"},
                                  timeout=2.0)
            else:
                with contextlib.suppress(Exception):
                    await hub.rpc(device, "show_text",
                                  {"title": "✓ 已唤醒",
                                   "text": "请讲，我在听…"}, timeout=2.0)
                log.info("[voice-t] listening shown +%.0fms",
                         (time.time()-t0)*1000)

            # Phase 2: CAPTURE
            t_cap = time.time()
            transcript = await _capture_utterance(hub, device,
                                                   max_record_s=max_record)
            log.info("[voice-t] capture turn=%d done +%.0fms transcript=%r",
                     turn, (time.time()-t_cap)*1000, transcript)
            if not transcript:
                # After a successful turn: silence ends the loop.
                if is_followup:
                    log.info("[voice] %s follow-up timed out, ending loop",
                             device.device_id)
                    break
                # Otherwise this is a recognition failure — count a strike
                # and re-listen up to ASR_RETRY_LIMIT times. The screen
                # already shows "识别失败 / 请再说一次" from _capture_utterance.
                asr_strikes += 1
                log.info("[voice] %s asr strike %d/%d",
                         device.device_id, asr_strikes, ASR_RETRY_LIMIT)
                if asr_strikes < ASR_RETRY_LIMIT:
                    await asyncio.sleep(0.6)   # let user read the prompt
                    continue
                # Exhausted retries → sad face and bail.
                log.info("[voice] %s asr exhausted, ending loop",
                         device.device_id)
                asyncio.create_task(_set_led(hub, device, *LED_ERROR))
                with contextlib.suppress(Exception):
                    await _ship_jpeg(device, screen.face_jpeg("sad"))
                await asyncio.sleep(0.8)
                return
            # success: reset strike counter and remember we got a real turn
            asr_strikes = 0
            had_success = True
            log.info("[voice] %s heard (turn %d): %s",
                     device.device_id, turn, transcript)
            # Record this user turn in the rolling dialog history that gets
            # passed to the next ASR session as `context_type=dialog_ctx`.
            # Newest-first per Volcengine spec; cap to DIALOG_HISTORY_MAX.
            device.dialog_history.insert(0, f"用户: {transcript}")
            del device.dialog_history[DIALOG_HISTORY_MAX:]

            # Phase 3a: ACK heard text. Brief flash before the long Hermes wait.
            asyncio.create_task(_set_led(hub, device, *LED_THINKING))
            with contextlib.suppress(Exception):
                await hub.rpc(device, "show_text",
                              {"title": "已听到 ✓", "text": transcript},
                              timeout=2.0)

            # Phase 3b: HERMES with elapsed-seconds ticker so the user knows
            # we're still working (Hermes typically 10–30 s; without a ticker
            # the screen looks frozen and the user fires a new wake event).
            hermes_done = asyncio.Event()
            hermes_t0 = time.time()
            async def hermes_ticker() -> None:
                while not hermes_done.is_set():
                    try:
                        await asyncio.wait_for(hermes_done.wait(), timeout=2.0)
                        return
                    except asyncio.TimeoutError:
                        pass
                    elapsed = int(time.time() - hermes_t0)
                    with contextlib.suppress(Exception):
                        await hub.rpc(device, "show_text",
                                      {"title": f"思考中… {elapsed}秒",
                                       "text": transcript}, timeout=2.0)
            ticker_task = asyncio.create_task(hermes_ticker())
            try:
                reply = await _ask_hermes(transcript)
            finally:
                hermes_done.set()
                with contextlib.suppress(Exception, asyncio.CancelledError):
                    await asyncio.wait_for(ticker_task, timeout=1.0)
            reply = (reply or "").strip() or "嗯,我现在不太能回答这个。"
            log.info("[voice] %s reply: %s", device.device_id, reply[:160])
            # Record bot turn so the next ASR session sees the full Q&A.
            device.dialog_history.insert(0, f"助手: {reply}")
            del device.dialog_history[DIALOG_HISTORY_MAX:]

            # Phase 4a: PREPARING TTS — LED only. The old "合成语音…"
            # screen was dead code: it was overwritten by "回复" below
            # before any await separated them, so the user never saw it.
            asyncio.create_task(_set_led(hub, device, *LED_TALKING))
            # Phase 4b: SPEAKING — pipe Doubao chunks straight to the
            # device. The unidirectional endpoint streams the first PCM
            # frame in ~400-800 ms; stream_pcm_async ships it the instant
            # it arrives instead of waiting for the entire reply to
            # render (~1.5 s for a 5 s utterance). That ~1 s saving is
            # the entire reason we picked the chunked endpoint.
            with contextlib.suppress(Exception):
                await hub.rpc(device, "show_text",
                              {"title": "回复", "text": reply}, timeout=2.0)
            _appid, _token = doubao_tts._load_creds()
            _tts_cfg = bridge_config.get_tts()
            _speaker = _tts_cfg.speaker or doubao_tts_unidirectional.DEFAULT_SPEAKER
            tts_iter = doubao_tts_unidirectional.synthesize_stream(
                reply,
                app_key=_appid, access_key=_token,
                speaker=_speaker,
                sample_rate=16000,
                audio_format="pcm",
                speech_rate=_tts_cfg.speech_rate,
                loudness_rate=_tts_cfg.loudness_rate,
                uid=device.device_id,
            )
            try:
                device.stream_task = asyncio.current_task()
                await stream_pcm_async(device, tts_iter, 16000)
            except asyncio.CancelledError:
                cancelled = True
                log.info("[voice] %s stream cancelled (wake interrupt)",
                         device.device_id)
                raise
            except Exception:
                log.exception("[voice] stream_pcm failed")
            finally:
                device.stream_task = None
                # Explicit aclose() so the underlying httpx.Response /
                # TCP connection is released *now*, even if the consumer
                # broke early (cancellation). Without this we'd rely on
                # GC for prompt cleanup, which is fragile under stress.
                with contextlib.suppress(Exception):
                    await tts_iter.aclose()

            # stream_pcm already blocked until t0 + audio_duration + 0.4s
            # margin, so the speaker has truly drained by now. We no longer
            # wait on the device's `tts.done` event — it lags 1-10 s
            # inconsistently and used to make us either (a) cut off audio
            # by opening mic too early or (b) hang the loop for 20 s. The
            # event is still fired by the device and observed by the wake
            # handler, just not gating voice_loop anymore.
            device.tts_active = False

            # Loop back: open mic again for an automatic follow-up turn.
            # User can say something more without re-uttering "Hi Wall-E".
            # Silence times out after 5 s and ends the loop normally.
    except asyncio.CancelledError:
        cancelled = True
        log.info("[voice] %s loop cancelled", device.device_id)
        raise
    finally:
        if not cancelled:
            await _voice_cleanup(hub, device)


async def _voice_cleanup(hub: Hub, device: Device) -> None:
    await asyncio.sleep(0.4)
    asyncio.create_task(_set_led(hub, device, *LED_IDLE))
    with contextlib.suppress(Exception):
        await _ship_jpeg(device, screen.face_jpeg("idle"))
    with contextlib.suppress(Exception):
        await hub.rpc(device, "wake_resume", {}, timeout=2.0)
    log.info("[voice] %s loop done, wake re-armed", device.device_id)


async def _capture_utterance(hub: Hub, device: Device,
                              max_record_s: float = VOICE_MAX_RECORD_S) -> str:
    """Capture mic PCM, then one-shot transcribe via Doubao Flash ASR.

    Why this rewrite (replaced the bigmodel_async WebSocket session):
      * Flash is a synchronous REST call — no per-packet pacing, no 8 s
        "等包超时" foot-guns, no session that dies if the user is silent.
      * For utterance-length audio (<10 s) Doubao returns the full
        transcript in ~500–2000 ms. Plenty fast for our turn-based UX.
      * Empty / failed transcripts simply re-POST; the API is idempotent.
      * No need for the IIR notch filter that destroyed the streaming
        recogniser, no noise primer, no speech-gating in handle_binary.

    Flow:
      1) mic_start RPC → device starts streaming PCM.
      2) Bridge accumulates into device.mic_dump and tracks RMS over the
         last 250 ms to detect end-of-speech.
      3) End conditions:
           * elapsed > max_record_s          → hard cap
           * voiced segment seen AND no voice for PARTIAL_QUIET_S → stop
           * no voiced segment by PRE_SPEECH_TIMEOUT_S → user never spoke
      4) mic_stop RPC.
      5) If captured ≥ 0.5 s, POST to flash; otherwise show "没听清".
      6) Show "识别中…" while flash runs, then show the transcript.
    """
    # End-of-speech timing — these are bridge-side only now; no Doubao
    # streaming pacing involved.
    PARTIAL_QUIET_S       = 1.4    # silence after last voice burst → stop
    PRE_SPEECH_TIMEOUT_S  = 5.0    # no speech detected → stop
    MIN_RECORD_S          = 0.6
    ENERGY_RMS_THRESHOLD  = 1000   # raw mic baseline ~700; speech >1500
    ENERGY_WINDOW_MS      = 250

    appid, token = doubao_tts._load_creds()

    # --- 1. Start mic capture (NO Doubao session opened up-front any more).
    device.mic_dump = bytearray()
    sid            = device.next_sid()
    device.asr_sid = sid
    try:
        await hub.rpc(device, "mic_start", {"sid": sid}, timeout=4.0)
    except Exception:
        log.exception("[voice] mic_start RPC failed")
        device.mic_dump = None
        device.asr_sid = 0
        with contextlib.suppress(Exception):
            await hub.rpc(device, "show_text",
                          {"title": "麦克风启动失败",
                           "text": "请稍后再试"}, timeout=2.0)
        return ""

    # --- 2. Record loop with RMS-based VAD (very small, no model).
    t_started        = time.time()
    last_voice_at    = 0.0
    first_voice_at   = 0.0
    energy_bytes     = 16000 * 2 * ENERGY_WINDOW_MS // 1000
    quiet_warned     = False
    try:
        while True:
            await asyncio.sleep(0.05)
            now     = time.time()
            elapsed = now - t_started
            if elapsed > max_record_s:
                log.info("[voice] capture hit max=%.1fs", max_record_s)
                break
            if device.mic_dump is None:
                break

            # RMS of the last ENERGY_WINDOW_MS of audio.
            if len(device.mic_dump) >= 64:
                tail = bytes(device.mic_dump[-energy_bytes:])
                arr  = np.frombuffer(tail, dtype="<i2").astype(np.float32)
                if arr.size:
                    rms = float(np.sqrt(np.mean(arr * arr)))
                    if rms >= ENERGY_RMS_THRESHOLD:
                        last_voice_at = now
                        if first_voice_at == 0.0:
                            first_voice_at = now
                            log.info("[voice] speech start +%.2fs", elapsed)

            if first_voice_at:
                # Speech began — wait for sustained silence.
                if (now - last_voice_at) >= PARTIAL_QUIET_S \
                        and elapsed >= MIN_RECORD_S:
                    log.info("[voice] quiet %.2fs after speech → stop",
                             now - last_voice_at)
                    break
            else:
                # No speech yet — warn at 2 s, give up at PRE_SPEECH_TIMEOUT_S.
                if elapsed > 2.0 and not quiet_warned:
                    quiet_warned = True
                    with contextlib.suppress(Exception):
                        await hub.rpc(device, "show_text",
                                      {"title": "听不到 / 请大声点",
                                       "text": "请讲，我在听…"}, timeout=2.0)
                if elapsed > PRE_SPEECH_TIMEOUT_S:
                    log.info("[voice] no speech in %.1fs → stop",
                             PRE_SPEECH_TIMEOUT_S)
                    break
    finally:
        with contextlib.suppress(Exception):
            await hub.rpc(device, "mic_stop", None, timeout=2.0)
        device.asr_sid = 0

    pcm     = bytes(device.mic_dump or b"")
    pcm_len = len(pcm)
    device.mic_dump = None
    if pcm_len < 16000:    # < 0.5 s
        log.info("[voice] capture too short (%d bytes)", pcm_len)
        with contextlib.suppress(Exception):
            await hub.rpc(device, "show_text",
                          {"title": "没听清",
                           "text": "请再说一次"}, timeout=2.0)
        return ""

    # --- 3. Flash ASR.
    log.info("[voice] capture %d bytes (%.1f s) → flash asr",
             pcm_len, pcm_len / 32000)
    with contextlib.suppress(Exception):
        await hub.rpc(device, "show_text",
                      {"title": "识别中…",
                       "text": f"{pcm_len//32000} 秒语音"}, timeout=1.5)
    try:
        result = await doubao_asr_flash.transcribe(
            pcm, app_key=appid, access_key=token,
            sample_rate=16000,
            enable_punc=True, enable_itn=True, enable_ddc=True,
        )
        transcript = result.text.strip()
        log.info("[voice] flash asr -> %r (%dms audio, %d utts)",
                 transcript, result.duration_ms, len(result.utterances))
    except doubao_asr_flash.FlashError as e:
        log.warning("[voice] flash asr error: %s", e)
        transcript = ""
    except Exception:
        log.exception("[voice] flash asr exception")
        transcript = ""
    return transcript


async def _ask_hermes(text: str) -> str:
    """One-shot Hermes invocation. The bridge runs on the same host as Hermes
    (the Mac mini), so we spawn `hermes -z` locally."""
    cmd = ["hermes", "-z", text, "--skills", HERMES_SKILLS, "--yolo"]
    log.info("[hermes] -> %s", " ".join(cmd[:3]))
    try:
        proc = await asyncio.create_subprocess_exec(
            *cmd,
            stdout=asyncio.subprocess.PIPE,
            stderr=asyncio.subprocess.PIPE,
        )
    except FileNotFoundError:
        log.error("[hermes] CLI not found in PATH")
        return ""
    try:
        out, err = await asyncio.wait_for(proc.communicate(), timeout=HERMES_TIMEOUT_S)
    except asyncio.TimeoutError:
        proc.kill()
        log.warning("[hermes] timeout after %.1fs", HERMES_TIMEOUT_S)
        return ""
    if proc.returncode != 0:
        log.warning("[hermes] exit=%d stderr=%s",
                    proc.returncode, (err or b"").decode(errors="replace")[:200])
    return (out or b"").decode("utf-8", errors="replace").strip()


async def http_voice(req: web.Request) -> web.Response:
    """POST /voice {device?} — manually trigger the voice loop (testing aid)."""
    hub: Hub = req.app["hub"]
    try:
        body = await req.json()
    except Exception:
        body = {}
    device = hub.get(body.get("device"))
    if device is None:
        return web.json_response({"ok": False, "error": "no device"}, status=404)
    asyncio.create_task(voice_loop(hub, device))
    return web.json_response({"ok": True, "device": device.device_id})


async def http_face(req: web.Request) -> web.Response:
    """POST /face {expr, device?}

    expr ∈ idle | happy | sad | surprised | talking | listening |
            sleepy | blink | thinking | alert
    Renders a vector robot face on the bridge with PIL and ships it as
    one JPEG over the existing KIND_DISP_IMG path. Cached per-expression,
    so repeated calls don't re-render."""
    hub: Hub = req.app["hub"]
    try:
        body = await req.json()
    except Exception:
        body = {}
    device = hub.get(body.get("device"))
    if device is None:
        return web.json_response({"ok": False, "error": "no device"}, status=404)
    expr = body.get("expr") or "idle"
    jpeg = screen.face_jpeg(expr)
    with contextlib.suppress(Exception):
        await _ship_jpeg(device, jpeg)
    return web.json_response({"ok": True, "device": device.device_id,
                              "expr": expr, "bytes": len(jpeg)})


async def http_show_text2(req: web.Request) -> web.Response:
    """POST /show_text {text, title?, face?, bg?, fg?, size?, device?}

    Renders text on the bridge with PIL (Chinese-capable). `face` is the
    name of a built-in face to inset in the upper-left corner. `bg`/`fg`
    are RGB tuples like [10, 20, 40]. `size` ∈ small/medium/large/auto.
    """
    hub: Hub = req.app["hub"]
    try:
        body = await req.json()
    except Exception:
        body = {}
    device = hub.get(body.get("device"))
    if device is None:
        return web.json_response({"ok": False, "error": "no device"}, status=404)
    text = (body.get("text") or "").strip()
    if not text:
        return web.json_response({"ok": False, "error": "missing text"}, status=400)
    title = body.get("title", "") or ""
    face = body.get("face")
    bg = tuple(body.get("bg") or screen.BG_DEFAULT)
    fg = tuple(body.get("fg") or screen.FG_DEFAULT)
    size = body.get("size", "auto")
    try:
        img = screen.render_text(text, title=title, face=face,
                                 bg=bg, fg=fg, size=size)
        jpeg = screen.to_jpeg(img, quality=80)
    except Exception as e:
        return web.json_response({"ok": False, "error": f"render: {e}"}, status=500)
    with contextlib.suppress(Exception):
        await _ship_jpeg(device, jpeg)
    return web.json_response({"ok": True, "device": device.device_id,
                              "bytes": len(jpeg)})


async def http_perform(req: web.Request) -> web.Response:
    """POST /perform {text, cues?, voice?, speed?, sample_rate?, device?}

    "Recite a poem with gestures." Synthesizes the FULL `text` in a single
    Doubao TTS call (no per-line gaps), then concurrently streams the audio
    to the device while firing scheduled `cues` on a timer.

    `cues` is a list of {at_ms, motion?, led?, expr?}. Timing is measured from
    the moment audio streaming begins — so `at_ms=0` fires as the first
    syllable starts. Motion is dispatched as a non-blocking RPC (do_action is
    now async on-device); LED + face are quick I2C ops that don't disrupt
    audio. Example cue:
        {"at_ms": 1500, "led":{"r":0,"g":255,"b":0}, "motion":"nod"}
    """
    hub: Hub = req.app["hub"]
    try:
        body = await req.json()
    except Exception:
        body = {}
    text = (body.get("text") or "").strip()
    if not text:
        return web.json_response({"ok": False, "error": "missing text"}, status=400)
    device = hub.get(body.get("device"))
    if device is None:
        return web.json_response({"ok": False, "error": "no device"}, status=404)
    sr = int(body.get("sample_rate", 16000))
    cues = body.get("cues") or []

    try:
        pcm = await doubao_tts.synthesize(
            text, voice=body.get("voice", doubao_tts.DEFAULT_VOICE),
            sample_rate=sr, speed=float(body.get("speed", 1.0)))
    except Exception as e:
        return web.json_response({"ok": False, "error": f"tts: {e}"}, status=502)

    loop = asyncio.get_event_loop()

    async def fire_cue(cue: dict, t0: float) -> None:
        delay = (cue.get("at_ms", 0) / 1000.0) - (loop.time() - t0)
        if delay > 0:
            await asyncio.sleep(delay)
        with contextlib.suppress(Exception):
            if "led" in cue:
                await hub.rpc(device, "set_led", cue["led"], timeout=2.0)
        # `text` / `face` go through the bridge-rendered display path
        # (PIL → JPEG → KIND_DISP_IMG). They render in parallel with the
        # streaming audio so the subtitle appears at the same instant the
        # corresponding line of TTS starts playing.
        if "text" in cue or "face" in cue:
            with contextlib.suppress(Exception):
                if cue.get("text"):
                    img = screen.render_text(
                        cue["text"], title=cue.get("title", ""),
                        face=cue.get("face"))
                    await _ship_jpeg(device, screen.to_jpeg(img, quality=80))
                elif cue.get("face"):
                    await _ship_jpeg(device, screen.face_jpeg(cue["face"]))
        if "motion" in cue:
            # fire-and-forget: don't await — motion runs ~1.2s on device but we
            # don't want this to delay the next cue or audio streaming
            loop.create_task(_fire_motion(hub, device, cue["motion"]))

    t0 = loop.time()
    cue_tasks = [loop.create_task(fire_cue(c, t0)) for c in cues]
    try:
        sent = await stream_pcm(device, pcm, sr)
    except Exception as e:
        for t in cue_tasks: t.cancel()
        return web.json_response({"ok": False, "error": f"stream: {e}"}, status=502)
    if cue_tasks:
        await asyncio.gather(*cue_tasks, return_exceptions=True)

    # Restore the default idle face once the performance is done (after a
    # short pause so the last subtitle stays visible briefly).
    async def _restore_idle() -> None:
        await asyncio.sleep(1.5)
        with contextlib.suppress(Exception):
            await _ship_jpeg(device, screen.face_jpeg("idle"))
    loop.create_task(_restore_idle())

    return web.json_response({
        "ok": True, "device": device.device_id,
        "bytes": sent, "est_ms": (len(pcm) // 2) * 1000 // sr,
        "cues_fired": len(cues),
    })


async def _fire_motion(hub: Hub, device: Device, name: str) -> None:
    with contextlib.suppress(Exception):
        await hub.rpc(device, "do_action", {"name": name}, timeout=2.0)


async def http_say(req: web.Request) -> web.Response:
    """POST /say {text, device?, voice?, sample_rate?, speaker?}
    -> synthesize via Doubao TTS unidirectional (HTTP Chunked), pipe PCM
    straight to device as chunks arrive, return when stream ends.

    Default path now uses the new chunked endpoint + stream_pcm_async so
    we get TTFB ~600-900 ms (first audio playing on device) instead of
    blocking on the REST one-shot until full synth (~2 s for 5 s reply).

    Set `legacy=1` to fall back to the old REST one-shot synth + bytes
    stream_pcm — useful for A/B testing the latency difference."""
    hub: Hub = req.app["hub"]
    try:
        body = await req.json()
    except Exception:
        body = {}
    text = (body.get("text") or "").strip()
    if not text:
        return web.json_response({"ok": False, "error": "missing text"}, status=400)
    device = hub.get(body.get("device"))
    if device is None:
        return web.json_response({"ok": False, "error": "no device"}, status=404)
    sr = int(body.get("sample_rate", 16000))
    if body.get("legacy"):
        try:
            pcm = await doubao_tts.synthesize(text,
                                              voice=body.get("voice", doubao_tts.DEFAULT_VOICE),
                                              sample_rate=sr,
                                              speed=float(body.get("speed", 1.0)))
        except Exception as e:
            return web.json_response({"ok": False, "error": f"tts: {e}"}, status=502)
        try:
            sent = await stream_pcm(device, pcm, sr)
        except Exception as e:
            return web.json_response({"ok": False, "error": f"stream: {e}"}, status=502)
        return web.json_response({"ok": True, "device": device.device_id,
                                  "bytes": sent, "sample_rate": sr,
                                  "mode": "legacy",
                                  "est_ms": (len(pcm) // 2) * 1000 // sr})
    appid, token = doubao_tts._load_creds()
    tts_cfg = bridge_config.get_tts()
    speaker = (body.get("speaker") or tts_cfg.speaker
               or doubao_tts_unidirectional.DEFAULT_SPEAKER)
    tts_iter = doubao_tts_unidirectional.synthesize_stream(
        text,
        app_key=appid, access_key=token,
        speaker=speaker,
        sample_rate=sr,
        audio_format="pcm",
        speech_rate=tts_cfg.speech_rate,
        loudness_rate=tts_cfg.loudness_rate,
        uid=device.device_id,
    )
    try:
        sent = await stream_pcm_async(device, tts_iter, sr)
    except Exception as e:
        log.exception("[say] streaming TTS failed")
        return web.json_response({"ok": False, "error": f"tts/stream: {e}"}, status=502)
    finally:
        # Same cleanup as voice_loop: prompt aclose() instead of relying on
        # GC, so the httpx connection to openspeech is released right away.
        with contextlib.suppress(Exception):
            await tts_iter.aclose()
    return web.json_response({"ok": True, "device": device.device_id,
                              "bytes": sent, "sample_rate": sr,
                              "mode": "streaming",
                              "est_ms": (sent // 2) * 1000 // sr})


async def http_listen(req: web.Request) -> web.Response:
    """POST /listen {device?, duration_s?} -> capture mic, run Doubao ASR,
    return final transcript.

    The device captures for `duration_s` (default 5) seconds, then bridge sends
    mic_stop. ASR finalizes within ~1s. Long-running but synchronous from the
    HTTP caller's perspective.
    """
    hub: Hub = req.app["hub"]
    try:
        body = await req.json()
    except Exception:
        body = {}
    device = hub.get(body.get("device"))
    if device is None:
        return web.json_response({"ok": False, "error": "no device"}, status=404)
    duration = float(body.get("duration_s", 5.0))
    appid, token = doubao_tts._load_creds()

    if device.asr is not None:
        return web.json_response({"ok": False, "error": "asr already active"}, status=409)

    dump_pcm = bool(body.get("dump", False))
    if dump_pcm:
        device.mic_dump = bytearray()

    sid = device.next_sid()
    try:
        await hub.rpc(device, "mic_start", {"sid": sid}, timeout=4.0)
    except Exception as e:
        return web.json_response({"ok": False, "error": f"mic_start: {e}"}, status=502)

    partials: list[str] = []
    finals: list[str] = []
    async def on_partial(text, _utts):
        partials.append(text)
        log.info("[asr partial] %s", text)
    async def on_final(text, _utts):
        finals.append(text)
        log.info("[asr final-utt] %s", text)

    sess = doubao_asr.DoubaoAsrSession(
        app_key=appid, access_key=token,
        on_partial=on_partial, on_final=on_final, uid=device.device_id,
    )
    try:
        await sess.start()
    except Exception as e:
        with contextlib.suppress(Exception):
            await hub.rpc(device, "mic_stop", None, timeout=2.0)
        return web.json_response({"ok": False, "error": f"asr_start: {e}"}, status=502)

    device.asr = sess
    device.asr_sid = sid

    try:
        await asyncio.sleep(duration)
    finally:
        with contextlib.suppress(Exception):
            await hub.rpc(device, "mic_stop", None, timeout=3.0)
        await asyncio.sleep(0.2)                     # let trailing pcm land
        device.asr = None
        device.asr_sid = 0
        with contextlib.suppress(Exception):
            await sess.feed(b"", is_last=True)
        transcript = await sess.wait_finish(timeout=8.0)

    dump_info = None
    if device.mic_dump is not None:
        pcm = bytes(device.mic_dump)
        device.mic_dump = None
        # write a 16k mono 16-bit WAV to /tmp for inspection
        import struct as _s
        path = f"/tmp/hotdog-mic-{int(time.time())}.wav"
        with open(path, "wb") as f:
            data_size = len(pcm)
            f.write(b"RIFF" + _s.pack("<I", 36 + data_size) + b"WAVE")
            f.write(b"fmt " + _s.pack("<IHHIIHH", 16, 1, 1, 16000, 32000, 2, 16))
            f.write(b"data" + _s.pack("<I", data_size) + pcm)
        # report the peak + RMS so we can tell silence vs real audio at a glance
        rms = 0
        if data_size >= 2:
            n = data_size // 2
            samples = _s.unpack(f"<{n}h", pcm)
            total = sum(s * s for s in samples)
            rms = int((total / n) ** 0.5) if n else 0
        dump_info = {"wav": path, "bytes": data_size, "rms": rms}
        log.info("[mic] dumped %d bytes to %s, rms=%d", data_size, path, rms)

    return web.json_response({
        "ok": True, "device": device.device_id,
        "transcript": transcript,
        "partials": partials,
        "duration_s": duration,
        "dump": dump_info,
    })


def _fit_to_display(img, w=320, h=240, bg=(0, 0, 0)):
    """Resize `img` to fit within w×h while preserving aspect ratio, then
    paste onto a black w×h canvas. Returns a Pillow Image in RGB mode."""
    from PIL import Image  # imported lazily to keep startup snappy
    img = img.convert("RGB")
    # Auto-level: dim camera captures (GC0308 indoors at q=70 stays ~mean 60/255)
    # render as a muddy gray slab on the LCD. autocontrast(cutoff=2) chops the
    # top/bottom 2% of the histogram and stretches — already well-exposed inputs
    # are a no-op; dim inputs gain a lot of perceived contrast.
    from PIL import ImageOps
    img = ImageOps.autocontrast(img, cutoff=2)
    iw, ih = img.size
    scale = min(w / iw, h / ih)
    new_w, new_h = max(1, int(iw * scale)), max(1, int(ih * scale))
    img = img.resize((new_w, new_h), Image.LANCZOS)
    canvas = Image.new("RGB", (w, h), bg)
    canvas.paste(img, ((w - new_w) // 2, (h - new_h) // 2))
    return canvas


async def _stream_disp_image(device: Device, jpeg: bytes) -> None:
    """Send a single KIND_DISP_IMG binary frame to the device. The on-device
    JPEG decoder is happy with up to a few-tens-of-KB single frame, so we
    don't fragment yet."""
    sid = device.next_sid()
    await device.ws.send(encode_binary(KIND_DISP_IMG, sid, 0, jpeg))
    log.info("disp.image sid=%d device=%s %d bytes", sid, device.device_id, len(jpeg))


async def http_show_image(req: web.Request) -> web.Response:
    """POST /show_image — three input modes, in priority order:
      1. ?url=https://… — bridge fetches the URL, no body needed
      2. ?path=/abs/file.png — bridge reads a local file
      3. raw image bytes in the body (JPEG/PNG/WebP/anything PIL handles)
    Resizes / letterboxes to 320×240, encodes JPEG q=80, ships KIND_DISP_IMG
    to the device. Returns immediately after WS send."""
    hub: Hub = req.app["hub"]
    device = hub.get(req.rel_url.query.get("device"))
    if device is None:
        return web.json_response({"ok": False, "error": "no device"}, status=404)

    url = req.rel_url.query.get("url")
    path = req.rel_url.query.get("path")
    raw: bytes = b""
    if url:
        try:
            async with aiohttp.ClientSession() as s:
                async with s.get(url, timeout=aiohttp.ClientTimeout(total=10)) as r:
                    if not r.ok:
                        return web.json_response(
                            {"ok": False, "error": f"fetch {url}: HTTP {r.status}"},
                            status=502)
                    raw = await r.read()
        except Exception as e:
            return web.json_response(
                {"ok": False, "error": f"fetch {url}: {e}"}, status=502)
    elif path:
        try:
            with open(path, "rb") as f:
                raw = f.read()
        except Exception as e:
            return web.json_response(
                {"ok": False, "error": f"read {path}: {e}"}, status=400)
    else:
        raw = await req.read()
    if not raw:
        return web.json_response({"ok": False, "error": "no image data"}, status=400)
    try:
        from PIL import Image
        import io
        img = Image.open(io.BytesIO(raw))
        fit = _fit_to_display(img)
        out = io.BytesIO()
        fit.save(out, format="JPEG", quality=80)
        jpeg = out.getvalue()
    except Exception as e:
        return web.json_response({"ok": False, "error": f"decode: {e}"}, status=400)
    try:
        await _stream_disp_image(device, jpeg)
    except Exception as e:
        return web.json_response({"ok": False, "error": f"send: {e}"}, status=502)
    return web.json_response({"ok": True, "device": device.device_id,
                              "bytes": len(jpeg),
                              "source": "url" if url else "path" if path else "body"})


async def http_find_face(req: web.Request) -> web.Response:
    """POST /find_face {device?, max_steps?}  -> one-shot face scan + lock."""
    hub: Hub = req.app["hub"]
    try:
        body = await req.json()
    except Exception:
        body = {}
    device = hub.get(body.get("device"))
    if device is None:
        return web.json_response({"ok": False, "error": "no device"}, status=404)
    if device.tracker is not None:
        return web.json_response({"ok": False, "error": "tracker active — stop it first"},
                                 status=409)
    try:
        result = await face_tracker.find_face(hub, device,
                                              max_steps=int(body.get("max_steps", 12)))
    except Exception as e:
        return web.json_response({"ok": False, "error": str(e)}, status=502)
    return web.json_response({"ok": True, "device": device.device_id, **result})


async def http_track(req: web.Request) -> web.Response:
    """POST /track {device?, action}  action ∈ start|stop|status"""
    hub: Hub = req.app["hub"]
    try:
        body = await req.json()
    except Exception:
        body = {}
    device = hub.get(body.get("device"))
    if device is None:
        return web.json_response({"ok": False, "error": "no device"}, status=404)
    action = (body.get("action") or "status").lower()
    if action == "start":
        if device.tracker is None:
            device.tracker = face_tracker.Tracker(hub=hub, device=device)
        device.tracker.start()
        return web.json_response({"ok": True, "tracking": True})
    if action == "stop":
        if device.tracker is not None:
            device.tracker.stop()
        device.tracker = None
        return web.json_response({"ok": True, "tracking": False})
    # status
    return web.json_response({"ok": True, "tracking": device.tracker is not None})


async def http_capture(req: web.Request) -> web.Response:
    """POST /capture {device?, quality?, format?}  -> JPEG bytes (or base64 wrap).

    Issues a cam_capture RPC to the device. The device snaps one frame,
    JPEG-encodes it, and pushes the bytes back as a KIND_CAM_JPEG binary
    frame keyed by sid. We await both the binary and the RPC res, return
    the JPEG. format=json wraps in {ok, bytes, w, h, jpeg_b64}; default
    returns raw image/jpeg.
    """
    hub: Hub = req.app["hub"]
    try:
        body = await req.json()
    except Exception:
        body = {}
    device = hub.get(body.get("device"))
    if device is None:
        return web.json_response({"ok": False, "error": "no device"}, status=404)
    quality = int(body.get("quality", 85))
    out_fmt = (body.get("format") or req.rel_url.query.get("format") or "binary").lower()

    sid = device.next_sid()
    loop = asyncio.get_event_loop()
    fut: asyncio.Future = loop.create_future()
    device.pending_jpegs[sid] = fut

    try:
        meta = await hub.rpc(device, "cam_capture",
                             {"sid": sid, "quality": quality}, timeout=10.0)
    except Exception as e:
        device.pending_jpegs.pop(sid, None)
        return web.json_response({"ok": False, "error": f"cam_capture: {e}"}, status=502)

    try:
        jpeg = await asyncio.wait_for(fut, timeout=6.0)
    except asyncio.TimeoutError:
        device.pending_jpegs.pop(sid, None)
        return web.json_response({"ok": False, "error": "jpeg frame timeout"}, status=504)

    if out_fmt == "json":
        import base64
        return web.json_response({
            "ok": True, "device": device.device_id,
            "bytes": len(jpeg), "w": meta.get("w"), "h": meta.get("h"),
            "jpeg_b64": base64.b64encode(jpeg).decode(),
        })
    return web.Response(body=jpeg, content_type="image/jpeg",
                        headers={"X-Hotdog-Width": str(meta.get("w", "")),
                                 "X-Hotdog-Height": str(meta.get("h", ""))})


async def http_play_pcm(req: web.Request) -> web.Response:
    """POST /play_pcm  body: raw PCM16LE mono bytes (no header).
    Query: ?sample_rate=16000&device=<id>"""
    hub: Hub = req.app["hub"]
    sr = int(req.rel_url.query.get("sample_rate", "16000"))
    device = hub.get(req.rel_url.query.get("device"))
    if device is None:
        return web.json_response({"ok": False, "error": "no device"}, status=404)
    pcm = await req.read()
    if not pcm:
        return web.json_response({"ok": False, "error": "empty pcm"}, status=400)
    try:
        sent = await stream_pcm(device, pcm, sr)
    except Exception as e:
        return web.json_response({"ok": False, "error": str(e)}, status=502)
    return web.json_response({"ok": True, "device": device.device_id, "bytes": sent})


async def http_rpc(req: web.Request) -> web.Response:
    """POST /rpc {device?, method, params?, timeout?} -> run an ocsc.v2 RPC."""
    hub: Hub = req.app["hub"]
    try:
        body = await req.json()
    except Exception:
        body = {}
    device = hub.get(body.get("device"))
    if device is None:
        return web.json_response({"ok": False, "error": "no device"}, status=404)
    method = body.get("method")
    if not method:
        return web.json_response({"ok": False, "error": "missing method"}, status=400)
    try:
        result = await hub.rpc(device, method, body.get("params"),
                               timeout=float(body.get("timeout", 6.0)))
    except asyncio.TimeoutError:
        return web.json_response({"ok": False, "error": "device timeout"}, status=504)
    except Exception as e:
        return web.json_response({"ok": False, "error": str(e)}, status=502)
    return web.json_response({"ok": True, "device": device.device_id, "result": result})


def build_http_app(hub: Hub) -> web.Application:
    app = web.Application()
    app["hub"] = hub
    app.router.add_get("/healthz", http_healthz)
    app.router.add_get("/status", http_status)
    app.router.add_post("/rpc", http_rpc)
    app.router.add_post("/say", http_say)
    app.router.add_post("/perform", http_perform)
    app.router.add_post("/face", http_face)
    app.router.add_post("/show_text", http_show_text2)
    app.router.add_post("/play_pcm", http_play_pcm)
    app.router.add_post("/listen", http_listen)
    app.router.add_post("/capture", http_capture)
    app.router.add_post("/show_image", http_show_image)
    app.router.add_post("/find_face", http_find_face)
    app.router.add_post("/track", http_track)
    app.router.add_post("/voice", http_voice)
    return app


# =========================================================================
# main
# =========================================================================

async def main_async(args: argparse.Namespace) -> None:
    hub = Hub()

    async def _ws_entry(ws):
        try:
            await ws_handler(ws, hub)
        except Exception:
            log.exception("[ws] handler crashed")

    # Bump keepalive from the websockets-12 defaults (20 s interval, 20 s
    # timeout). During a stream_pcm burst the device's audio task can starve
    # the ws ping handler — we saw real 20 s ping timeouts ending sessions
    # mid-utterance, then a "phantom" re-connect that looked like a stray
    # wake event. 40 s/60 s is comfortable on a healthy LAN.
    ws_server = await websockets.serve(_ws_entry, args.ws_host, args.ws_port,
                                       max_size=8 * 1024 * 1024,
                                       ping_interval=40, ping_timeout=60)
    log.info("[ws] ocsc.v2 listening on %s:%d", args.ws_host, args.ws_port)

    runner = web.AppRunner(build_http_app(hub))
    await runner.setup()
    await web.TCPSite(runner, args.http_host, args.http_port).start()
    log.info("[http] admin listening on %s:%d", args.http_host, args.http_port)

    # Pre-render the listening screen so the first wake-event doesn't pay
    # the PIL render cost (would otherwise be a one-time ~300 ms hiccup).
    try:
        screen.listening_screen_jpeg()
        screen.face_jpeg("idle")
        screen.face_jpeg("listening")
        screen.face_jpeg("thinking")
        screen.face_jpeg("talking")
        screen.face_jpeg("sad")
        log.info("[screen] pre-warmed cache")
    except Exception:
        log.exception("[screen] pre-warm failed")

    await asyncio.Event().wait()


def main() -> None:
    # Load config first so an early failure (missing config.toml) prints
    # a clean message before argparse / asyncio.run touch the network.
    cfg = bridge_config.load()
    ap = argparse.ArgumentParser(
        description="DesktopWallE bridge — config defaults come from "
                    "bridge/config.toml; CLI flags override per-run.")
    ap.add_argument("--ws-host",   default=cfg.ws.host)
    ap.add_argument("--ws-port",   type=int, default=cfg.ws.port)
    ap.add_argument("--http-host", default=cfg.http.host)
    ap.add_argument("--http-port", type=int, default=cfg.http.port)
    ap.add_argument("--log-level", default="INFO")
    args = ap.parse_args()
    logging.basicConfig(
        level=getattr(logging, args.log_level.upper()),
        format="%(asctime)s %(levelname)s %(name)s %(message)s",
    )
    _effective_speaker = cfg.tts.speaker or doubao_tts_unidirectional.DEFAULT_SPEAKER
    log.info("config: doubao=ok ws=%s:%d http=%s:%d tts.speaker=%s source=%s",
             args.ws_host, args.ws_port, args.http_host, args.http_port,
             _effective_speaker, cfg.source)
    try:
        asyncio.run(main_async(args))
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()

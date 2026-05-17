"""
ByteDance / Volcengine Doubao streaming ASR client (v3 SAUC bigmodel_async).

Protocol overview (verified against vocotype-cli, UltraEval-Audio, ai-app-lab):

- Auth = plain headers on WS upgrade (no Bearer, no signature):
    X-Api-App-Key:    <appid>
    X-Api-Access-Key: <access_token>
    X-Api-Resource-Id: volc.bigasr.sauc.duration
    X-Api-Request-Id: <uuid4>
- Binary frames carry a 4-byte header + optional 4-byte signed seq + 4-byte
  payload_size + gzip(payload).
- First frame is FULL_CLIENT_REQUEST with JSON config. Subsequent frames are
  AUDIO_ONLY_REQUEST. The very last audio frame uses flag NEG_WITH_SEQUENCE
  and a negative seq number — without this the session hangs until timeout.

Usage:

    sess = DoubaoAsrSession(app_key, access_key, on_partial=cb, on_final=cb)
    await sess.start()
    await sess.feed(pcm_bytes, is_last=False)
    ...
    await sess.feed(b"", is_last=True)
    await sess.wait_finish()
"""

from __future__ import annotations

import asyncio
import gzip
import json
import logging
import uuid
from dataclasses import dataclass, field
from typing import Any, Awaitable, Callable

import websockets

log = logging.getLogger("doubao_asr")

# Protocol constants
PROTO_VER = 0x1
HDR_SZ = 0x1

# Message types (high nibble of byte 1)
FULL_CLIENT_REQUEST = 0x1
AUDIO_ONLY_REQUEST = 0x2
FULL_SERVER_RESPONSE = 0x9
SERVER_ACK = 0xB
SERVER_ERROR_RESPONSE = 0xF

# Message flags (low nibble of byte 1)
FLAG_NO_SEQ = 0b0000
FLAG_POS_SEQ = 0b0001
FLAG_LAST = 0b0010
FLAG_NEG_WITH_SEQ = 0b0011

# Serialization / compression (byte 2)
JSON_SER = 0x1
GZIP_COMP = 0x1

DEFAULT_URL = "wss://openspeech.bytedance.com/api/v3/sauc/bigmodel_async"
DEFAULT_RESOURCE_ID = "volc.seedasr.sauc.duration"


def _header(msg_type: int, flags: int) -> bytes:
    return bytes([
        (PROTO_VER << 4) | HDR_SZ,
        (msg_type << 4) | flags,
        (JSON_SER << 4) | GZIP_COMP,
        0x00,
    ])


def _build_config_frame(cfg: dict, seq: int = 1) -> bytes:
    payload = gzip.compress(json.dumps(cfg, ensure_ascii=False).encode("utf-8"))
    return (_header(FULL_CLIENT_REQUEST, FLAG_POS_SEQ)
            + seq.to_bytes(4, "big", signed=True)
            + len(payload).to_bytes(4, "big")
            + payload)


def _build_audio_frame(pcm: bytes, seq: int, is_last: bool) -> bytes:
    flags = FLAG_NEG_WITH_SEQ if is_last else FLAG_POS_SEQ
    signed_seq = -seq if is_last else seq
    payload = gzip.compress(pcm) if pcm else gzip.compress(b"")
    return (_header(AUDIO_ONLY_REQUEST, flags)
            + signed_seq.to_bytes(4, "big", signed=True)
            + len(payload).to_bytes(4, "big")
            + payload)


def _parse_frame(buf: bytes) -> dict:
    hdr_size = buf[0] & 0x0F
    mt = buf[1] >> 4
    mf = buf[1] & 0x0F
    comp = buf[2] & 0x0F
    body = buf[hdr_size * 4:]
    out: dict[str, Any] = {"type": mt, "is_last": bool(mf & FLAG_LAST)}
    if mf & FLAG_POS_SEQ:
        out["seq"] = int.from_bytes(body[:4], "big", signed=True)
        body = body[4:]
    if mt == FULL_SERVER_RESPONSE or mt == SERVER_ACK:
        size = int.from_bytes(body[:4], "big", signed=True)
        body = body[4:4 + max(size, 0)]
    elif mt == SERVER_ERROR_RESPONSE:
        out["code"] = int.from_bytes(body[:4], "big", signed=False)
        size = int.from_bytes(body[4:8], "big", signed=False)
        body = body[8:8 + size]
    if comp == GZIP_COMP and body:
        try:
            body = gzip.decompress(body)
        except Exception:
            pass
    try:
        out["payload"] = json.loads(body.decode("utf-8")) if body else {}
    except Exception:
        out["payload_raw"] = body
    return out


PartialCb = Callable[[str, list[dict]], Awaitable[None]]
FinalCb = Callable[[str, list[dict]], Awaitable[None]]
ErrorCb = Callable[[Exception], Awaitable[None]]


@dataclass
class DoubaoAsrSession:
    app_key: str
    access_key: str
    on_partial: PartialCb | None = None
    on_final: FinalCb | None = None
    on_error: ErrorCb | None = None
    resource_id: str = DEFAULT_RESOURCE_ID
    url: str = DEFAULT_URL
    uid: str = "stackchan"
    end_window_ms: int = 800          # trailing-silence ms before finalize
    # Two-pass mode: streaming model emits live partials, non-streaming model
    # re-recognises each definite sentence for accuracy. Recommended for
    # bigmodel_async — gives both fast first character AND high-quality final.
    enable_nonstream: bool = False
    # ssd_version="200" turns on the large-model SSD (smarter sentence
    # boundaries / better punctuation) — recommended for Seed-ASR 2.0.
    ssd_version: str = ""
    # Dialog history fed to the recogniser as `context_type=dialog_ctx`. List
    # of utterance strings ordered NEWEST FIRST (per Doubao spec). The model
    # uses this to disambiguate names, jargon, follow-up references etc.
    # The Volcengine docs cap this at ~20 turns / 800 tokens — caller is
    # responsible for trimming.
    context: list[str] | None = None

    ws: Any = None
    seq: int = 1
    _recv_task: asyncio.Task | None = None
    _last_text: str = ""
    _final_text: str = ""
    # Accumulated utterances keyed by start_time (Doubao Seed-ASR rolls the
    # `result.text` field over per-utterance; we must keep them ourselves so
    # the full transcript survives long recordings with multiple sentences).
    _utterances: dict = field(default_factory=dict)
    _finished: asyncio.Event = field(default_factory=asyncio.Event)
    _closed: bool = False

    async def start(self) -> None:
        # Per official docs (大模型流式语音识别API.pdf p.2-3):
        # X-Api-Sequence is REQUIRED and 固定值 -1.
        # X-Api-Connect-Id is recommended (随机 UUID, used by server for tracing).
        headers = {
            "X-Api-App-Key": self.app_key,
            "X-Api-Access-Key": self.access_key,
            "X-Api-Resource-Id": self.resource_id,
            "X-Api-Request-Id": str(uuid.uuid4()),
            "X-Api-Connect-Id": str(uuid.uuid4()),
            "X-Api-Sequence": "-1",
        }
        cfg = {
            "user": {"uid": self.uid},
            "audio": {
                "format": "pcm",
                "codec": "raw",
                "rate": 16000,
                "bits": 16,
                "channel": 1,
            },
            "request": {
                "model_name": "bigmodel",
                "enable_punc": True,         # 标点符号
                "enable_itn": True,          # 数字/单位规范化
                "enable_ddc": False,         # 顺滑(去口头禅)— off, lets us keep "嗯" etc verbatim
                "show_utterances": True,
                "result_type": "full",
                "end_window_size": int(self.end_window_ms),
                "force_to_speech_time": 1000,
            },
        }
        # ssd_version is **opt-in** — passing the empty string makes Doubao
        # return blank transcripts. Likewise enable_nonstream changes the
        # server pacer to expect a single 'all audio at once' upload and
        # triggers 45000081 'Timeout waiting next packet' on streaming.
        if self.enable_nonstream:
            cfg["request"]["enable_nonstream"] = True
        if self.ssd_version:
            cfg["request"]["ssd_version"] = str(self.ssd_version)
        # Dialog context — per Volcengine docs the `context` field of
        # request.corpus is a JSON-encoded STRING (not a nested object).
        if self.context:
            ctx_obj = {
                "context_type": "dialog_ctx",
                "context_data": [{"text": t} for t in self.context if t],
            }
            cfg["request"].setdefault("corpus", {})["context"] = json.dumps(
                ctx_obj, ensure_ascii=False)
            log.info("[asr] dialog context: %d turns, %d chars",
                     len(ctx_obj["context_data"]),
                     sum(len(t) for t in self.context if t))
        # `additional_headers` is the modern keyword on websockets>=14.
        self.ws = await websockets.connect(
            self.url, additional_headers=headers,
            max_size=10 * 1024 * 1024, open_timeout=10,
        )
        import json as _j
        log.info('[asr] start cfg=%s', _j.dumps(cfg, ensure_ascii=False)[:400])
        await self.ws.send(_build_config_frame(cfg, seq=self.seq))
        ack_raw = await asyncio.wait_for(self.ws.recv(), timeout=10)
        ack = _parse_frame(ack_raw)
        if ack["type"] == SERVER_ERROR_RESPONSE:
            await self._safe_close()
            raise RuntimeError(f"asr init failed code={ack.get('code')} "
                               f"payload={ack.get('payload')}")
        log.debug("[asr] init ack: %s", ack)
        self._recv_task = asyncio.create_task(self._recv_loop())


    async def feed(self, pcm: bytes, is_last: bool = False) -> None:
        if self.ws is None or self._closed:
            return
        self.seq += 1
        frame = _build_audio_frame(pcm, self.seq, is_last)
        try:
            await self.ws.send(frame)
        except websockets.ConnectionClosed:
            self._closed = True
            self._finished.set()

    def _compose_full_text(self, current_text: str = "") -> str:
        """Concatenate every utterance we've seen so far, sorted by start_time.

        If the current packet's `text` doesn't match any utterance (e.g. it's
        the in-progress sentence we haven't yet stored), append it too.
        """
        sorted_utts = sorted(
            self._utterances.values(),
            key=lambda u: u.get("start_time", 0),
        )
        accum = "".join((u.get("text") or "") for u in sorted_utts)
        # If current_text isn't already covered, treat it as the live
        # (not-yet-definite) tail and append.
        if current_text and current_text not in accum:
            return accum + current_text
        return accum or current_text

    async def _recv_loop(self) -> None:
        try:
            async for raw in self.ws:
                if not isinstance(raw, (bytes, bytearray)):
                    continue
                f = _parse_frame(raw)
                if f["type"] == SERVER_ERROR_RESPONSE:
                    err = RuntimeError(f"asr server error code={f.get('code')} "
                                       f"msg={f.get('payload')}")
                    if self.on_error:
                        await self.on_error(err)
                    log.warning("[asr] %s", err)
                    self._finished.set()
                    return
                payload = f.get("payload") or {}
                result = payload.get("result") or {}
                text = result.get("text", "") or ""
                utts = result.get("utterances", []) or []
                # Record every utterance keyed by start_time — overwrite is
                # OK because Doubao refines the same utterance until it's
                # marked definite.
                for u in utts:
                    st = u.get("start_time", 0)
                    self._utterances[st] = u
                full = self._compose_full_text(text)
                if full and full != self._last_text:
                    self._last_text = full
                    if self.on_partial:
                        try:
                            await self.on_partial(full, utts)
                        except Exception:
                            log.exception("[asr] on_partial cb failed")
                # Per-sentence final cb (definite=true)
                for u in utts:
                    if u.get("definite") and self.on_final:
                        try:
                            await self.on_final(u.get("text", ""), [u])
                        except Exception:
                            log.exception("[asr] on_final cb failed")
                if f["is_last"]:
                    self._final_text = self._compose_full_text(text)
                    self._finished.set()
                    return
        except websockets.ConnectionClosed:
            self._finished.set()
        except Exception as e:
            if self.on_error:
                await self.on_error(e)
            log.exception("[asr] recv loop crashed")
            self._finished.set()

    async def wait_finish(self, timeout: float = 15.0) -> str:
        try:
            await asyncio.wait_for(self._finished.wait(), timeout=timeout)
        except asyncio.TimeoutError:
            pass
        await self._safe_close()
        return self._final_text or self._last_text

    async def _safe_close(self) -> None:
        if self._closed:
            return
        self._closed = True
        if self._recv_task and not self._recv_task.done():
            self._recv_task.cancel()
            try:
                await self._recv_task
            except (asyncio.CancelledError, Exception):
                pass
        if self.ws is not None:
            try:
                await self.ws.close()
            except Exception:
                pass

    @property
    def final_text(self) -> str:
        return self._final_text or self._last_text

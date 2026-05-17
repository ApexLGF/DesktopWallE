# bridge — DesktopWallE bridge daemon

Long-lived Python 3.13 process that mediates between the StackChan device firmware and an LLM agent host (Hermes, OpenClaw, or anything that speaks HTTP).

## Wire diagram

```
device (ESP32-S3)   ──ws://lan:8765 (ocsc.v2)──▶   bridge/server.py
                                                      │
              ┌───── HTTP /say, /rpc, /voice ◀────────┼──── Hermes / OpenClaw / curl
              │                                       │
              ▼                                       ▼
   Doubao /tts/unidirectional             Doubao /asr/.../flash
   (HTTP Chunked, NDJSON, gzip)           (REST one-shot)
```

## Files

| File | Purpose |
|------|---------|
| `server.py` | Main daemon — asyncio WS server (`:8765`) for devices + aiohttp admin (`:8766`) for the agent. Owns the device state, runs `voice_loop`, ships audio frames. |
| `protocol.py` | ocsc.v2 frame encoder/decoder. Binary header layout, all message types. |
| `doubao_asr_flash.py` | Flash ASR client — encodes raw int16 PCM → OGG/OPUS via ffmpeg, POSTs to `/api/v3/auc/bigmodel/recognize/flash`, retries on timeout/5xx. |
| `doubao_tts_unidirectional.py` | TTS client — opens an HTTP Chunked stream to `/api/v3/tts/unidirectional`, yields PCM chunks as they arrive. First chunk ~400–800 ms after POST. |
| `doubao_asr.py` | Legacy bidirectional streaming ASR (kept for fallback). |
| `doubao_tts.py` | Legacy REST one-shot TTS (kept for fallback + creds loader). |
| `display.py` | PIL-based on-bridge rendering for faces and text panels → RGB565 → device LCD. |
| `tracker.py` | Per-device face/person tracker state machine (scan → track → idle). Optional. |
| `mcp_server.py` | Wraps the admin HTTP API as an MCP server for tool-using LLM clients. |
| `test_doubao_new.py` | End-to-end self-test: text → TTS → PCM → Flash ASR → text == text. |

## Configure

Copy `config.toml.example` to either `~/.stackproxy/config.toml` (legacy path used by `_load_creds()`) or `bridge/config.toml` and set your Volcengine Doubao keys. You can also override via env:

```sh
export HOTDOG_DOUBAO_APPID=...
export HOTDOG_DOUBAO_ACCESS_TOKEN=...
```

## Run

```sh
python3.13 -m pip install -r requirements.txt
python3.13 server.py
```

Defaults:
- WebSocket: `0.0.0.0:8765` (device side)
- HTTP admin: `0.0.0.0:8766` (agent side)

Both can be changed via `config.toml` or CLI flags (see `python3.13 server.py --help`).

## Admin endpoints

| Method | Path | What it does |
|--------|------|--------------|
| GET | `/healthz` | Liveness probe. |
| GET | `/status` | List connected devices, recent events. |
| POST | `/rpc` | Forward an ocsc.v2 RPC to a device. Body `{device, method, params}`. |
| POST | `/say` | Synthesize text → stream PCM to device. Default uses chunked TTS + stream_pcm_async; pass `{"legacy":true}` to use REST one-shot. |
| POST | `/listen` | Open mic on device, run Flash ASR, return transcript. |
| POST | `/voice` | Trigger one full `voice_loop` iteration (listen → ASR → Hermes → TTS). |
| POST | `/show_text` | Render text via PIL, push as RGB565. |
| POST | `/face` | Show a cached face JPEG (`idle`, `happy`, `sad`, `surprised`, `thinking`, `listening`, `talking`, `sleepy`, `blink`, `alert`). |
| POST | `/play_pcm` | Stream a caller-supplied PCM blob to the device. |
| POST | `/perform` | Run a multi-step "performance" (face + LED + motion + speech composed). |
| POST | `/find_face` / `/track` | Toggle the auto-tracker. |

## Doubao endpoints reference

- ASR: `https://openspeech.bytedance.com/api/v3/auc/bigmodel/recognize/flash` — resource `volc.bigasr.auc_turbo`.
- TTS: `https://openspeech.bytedance.com/api/v3/tts/unidirectional` — resource `volc.service_type.10029`.

Both expect `X-Api-App-Key` (ASR) / `X-Api-App-Id` (TTS) + `X-Api-Access-Key` + `X-Api-Resource-Id` headers. Docs: [Volcengine speech service](https://www.volcengine.com/docs/6561).

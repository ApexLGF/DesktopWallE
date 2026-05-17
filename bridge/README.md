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

```sh
cp config.toml.example config.toml      # next to server.py
$EDITOR config.toml                     # fill in [doubao] appid + access_token
```

`config.toml` is the single source of truth. `bridge/config.py` reads it on startup and exposes typed accessors (`get_doubao()`, `get_ws()`, `get_http()`, `get_mcp()`, `get_agent()`). There are **no environment-variable fallbacks** — if you'd rather configure via env vars, set them in your shell init and put `${...}` references in the TOML by hand.

Loader priority:
1. `bridge/config.toml` (preferred — sits next to the code)
2. `~/.stackproxy/config.toml` (legacy — kept working for older installs)

Verify your config is correctly picked up:

```sh
python3.13 config.py
# source: /path/to/bridge/config.toml
# doubao.appid:        2380...2817 (len=10)
# doubao.access_token: PZ5z...0NuK (len=32)
# ws:    0.0.0.0:8765
# http:  0.0.0.0:8766
# ...
```

The file is in `.gitignore`. Your keys never leave your machine.

## Run

```sh
python3.13 -m pip install -r requirements.txt
python3.13 server.py
```

Defaults are read from `config.toml`. CLI flags (`--ws-port`, `--http-port`, ...) can override any single value per run without touching the file — useful for spinning up a second bridge during development.

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

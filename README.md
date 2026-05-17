# DesktopWallE

A LAN-resident desktop robot that combines an [M5Stack CoreS3 + StackChan](https://github.com/mongonta0716/stack-chan) hardware platform with a Python bridge daemon and an LLM agent. You wake it with a hotword, talk to it, and it talks back through Doubao (ByteDance Volcengine) ASR/TTS. Heading, eyes, LEDs, motors, camera, sensors — everything is steerable from the agent loop.

Inspired by Pixar's Wall‑E — small, expressive, and very chatty.

```
┌─────────────────────┐         ws://    ┌─────────────────────┐    https      ┌────────────────────┐
│  M5 CoreS3 + Stack  │  ◀──────────────▶│   bridge (Python)    │ ──────────▶ │  Doubao ASR Flash  │
│  Chan firmware      │  ocsc.v2 binary  │   server.py (asyncio)│ ──────────▶ │  Doubao TTS chunked│
│  (Arduino C++)      │   audio + RPC    │                      │             └────────────────────┘
└─────────────────────┘                  │  + Hermes / OpenClaw│
        │   I2S mic / I2S DAC            │    agent gateway     │
        │   M5.Speaker.playRaw           └──────────┬───────────┘
        ▼                                            │ HTTP
   speaker + screen + LEDs + servos                  ▼
                                              LLM (any provider)
```

## What is in this repo

| Path | Language | What it is |
|------|----------|------------|
| [`src/main.cpp`](src/main.cpp) | Arduino C++ | Firmware for the M5Stack CoreS3 + StackChan. Single TU. Exposes 34 HTTP endpoints + an ocsc.v2 WebSocket client. |
| [`StackChanOpenClaw/`](StackChanOpenClaw/) | Arduino CLI shim | One-line `.ino` so Arduino IDE / arduino-cli accepts the project. PlatformIO uses `src/` directly. |
| [`include/config.example.h`](include/config.example.h) | C header | Template for the per-device config (Wi-Fi, bridge host, optional auth token). Copy to `include/config.h`. |
| [`bridge/`](bridge/) | Python 3.13 | Long-lived daemon that holds the WebSocket connection to the device, runs Doubao Flash ASR + chunked TTS, and forwards transcripts to the agent. |
| [`bridge/doubao_asr_flash.py`](bridge/doubao_asr_flash.py) | Python | One-shot Flash ASR client (`/api/v3/auc/bigmodel/recognize/flash`). Encodes audio as OGG/OPUS @16 kbps to keep uploads small. |
| [`bridge/doubao_tts_unidirectional.py`](bridge/doubao_tts_unidirectional.py) | Python | HTTP Chunked unidirectional TTS client (`/api/v3/tts/unidirectional`). Streams PCM frames as they arrive — first audio chunk lands in ~400–800 ms. |
| [`bridge/test_doubao_new.py`](bridge/test_doubao_new.py) | Python | Self-test that round-trips text → TTS → PCM → Flash ASR → text and asserts equality. |
| [`hermes-stackchan-skill/`](hermes-stackchan-skill/) | Hermes skill | Installs into Hermes Agent so it can drive the device. |
| [`hermes-hotdog-skill/`](hermes-hotdog-skill/) + [`hermes-hotdog-plugin/`](hermes-hotdog-plugin/) | Hermes skill + plugin | Personality skin for the agent — turns the bridge into "热狗" (Hotdog) the desk robot. |
| [`openclaw-stackchan-skill/`](openclaw-stackchan-skill/) | OpenClaw skill | Same idea for the OpenClaw agent host. |
| [`tools/openclaw_stackchan_demo.sh`](tools/openclaw_stackchan_demo.sh) | Bash | Curl-driven smoke test that exercises every HTTP endpoint on the device. |

## Hardware

| Part | Notes |
|------|-------|
| M5Stack CoreS3 (ESP32-S3) | 240 MHz dual-core, 8 MB PSRAM, 16 MB flash, GC0308 camera, BMI270 IMU, BMM150 mag, AXP2101 PMU, AW9523 GPIO expander, 320×240 IPS LCD with capacitive touch, NS4168 I2S DAC + speaker, dual MEMS mics, USB-C, microSD. |
| StackChan base (servo type B) | 2× SG90 servos (yaw + pitch), capacitive touch headplate, 12-pixel RGB ring around the neck. The [Mongonta stack-chan](https://github.com/mongonta0716/stack-chan) repo is the upstream of the mechanics. |
| Optional | M5Stack speaker module if you want louder output; otherwise the onboard NS4168 is fine. |

## Quick start

### 1. Flash the firmware

```sh
cp include/config.example.h include/config.h
# Edit your 2.4 GHz Wi-Fi SSID/password.
# Set STACKPROXY_WS_HOST to the LAN IP of the machine that will run the bridge.

arduino-cli compile --fqbn m5stack:esp32:m5stack_cores3 \
    --build-path .arduino-build StackChanOpenClaw
arduino-cli upload  -p /dev/cu.usbmodemXXX --fqbn m5stack:esp32:m5stack_cores3 \
    --input-dir .arduino-build StackChanOpenClaw
arduino-cli monitor -p /dev/cu.usbmodemXXX -c baudrate=115200
```

PlatformIO works too:

```sh
pio run
pio run -t upload --upload-port /dev/cu.usbmodemXXX
pio device monitor -p /dev/cu.usbmodemXXX -b 115200
```

The device boots, joins Wi-Fi, advertises mDNS as `stackchan.local`, and opens a WebSocket to your bridge host. Tap its head; you should see `head.tap` events arrive at the bridge.

### 2. Run the bridge

The bridge is Python 3.13. It expects `ffmpeg` on the PATH for the OPUS encoder.

```sh
brew install ffmpeg python@3.13          # macOS; apt equivalents on Linux
cd bridge
python3.13 -m pip install -r requirements.txt
cp config.toml.example ~/.stackproxy/config.toml
# edit ~/.stackproxy/config.toml with your Doubao appid + access_token

python3.13 server.py
```

You should see:

```
[ws]    ocsc.v2 listening on 0.0.0.0:8765
[http]  admin listening on 0.0.0.0:8766
[ws]    hello <device-id> boot=N fw=1.0.0 caps=[...] remote=...
[disp]  idle face -> <device-id>
```

### 3. Smoke-test the audio path

```sh
DOUBAO_APP_KEY=...  DOUBAO_ACCESS_KEY=...  python3.13 bridge/test_doubao_new.py
```

Expected output:

```
--- round-trip: '今天天气真不错' ---
  TTS: 52292 B PCM (1.63s audio) in 921ms first=625ms chunks=10
  ASR: text='今天天气真不错。' duration=1634ms utts=1
...
--> 3/3 passed
```

Then poke the live audio path directly:

```sh
curl -X POST http://<bridge-host>:8766/say \
    -H 'Content-Type: application/json' \
    -d '{"text":"你好，我是热狗，今天有什么需要帮忙的吗？","device":"<device-id>"}'
```

The device should start speaking within ~600 ms of the POST.

### 4. (Optional) Plug in an agent

The bridge is just a protocol translator. Point it at any chat agent:
- **Hermes** — copy [`hermes-stackchan-skill/`](hermes-stackchan-skill/) into `~/.hermes/skills/`. The skill exposes `sc` commands like `sc say "hello"`, `sc face happy`, `sc move nod`.
- **OpenClaw** — copy [`openclaw-stackchan-skill/`](openclaw-stackchan-skill/) into `~/.openclaw/workspace/skills/`. Same idea via OpenClaw's tool API.
- **Anything else** — the bridge speaks HTTP. Wire it to your own LLM loop in 50 lines of Python.

## Architecture deep dive

### Why a bridge daemon (not on-device)?

Earlier iterations tried to run TLS + JSON + base64 + Volcengine signing on the ESP32 directly. The CoreS3 has the headroom but the stack-blow risk and per-call latency made the agent loop feel sluggish. Moving everything to a Mac/Linux box on the same LAN lets us:

- Reuse the Python ecosystem (httpx, websockets, asyncio) for fast iteration.
- Hold a single persistent ocsc.v2 WS to the device — no per-call TCP handshake.
- Stream audio over a binary WS frame (8-byte header + raw int16 LE PCM) at exactly 1× realtime so the device's I2S DMA never underruns.
- Make protocol upgrades (e.g. swapping TTS vendors) a one-line change instead of an OTA.

### Why Flash ASR + OGG/OPUS encoding?

The naive "streaming Doubao bidirectional ASR" path turned out to be very fiddle-prone: the server requires you to feed packets at exactly the right rate or it kills the session with the dreaded `45000081 等包超时` error. Flash ASR (`/api/v3/auc/bigmodel/recognize/flash`) is a synchronous REST call — no pacing, no per-packet timer, no session state. For utterance-length audio (<10 s) it returns the full transcript in ~500–2000 ms.

The one catch: Flash takes the entire audio in one POST body. A 1.6 s WAV is ~70 KB → fine on most networks, but on bandwidth-shaped uplinks (mine maxes out at ~2 KB/s to Doubao) it would time out. **OGG/OPUS at 16 kbps brings the same 1.6 s of speech down to ~5 KB JSON** — uploads in ~1.5 s end-to-end. We use ffmpeg as the encoder (10–25 ms per call, negligible).

### Why HTTP Chunked TTS (not WebSocket)?

The unidirectional endpoint (`/api/v3/tts/unidirectional`) is `Transfer-Encoding: chunked` + gzip + NDJSON. Each line is `{"code":0,"data":"<base64-PCM>"}` plus a final sentence-end marker. `httpx.AsyncClient.stream` + `aiter_lines()` does all the work. We never see WebSocket framing, never have to track sequence numbers, never have to gzip-decode by hand. First audio chunk lands ~400 ms after the POST and the entire 5 s reply ships in ~1.5 s.

The bridge then pipes those chunks straight to the device via `stream_pcm_async()` — see [`bridge/server.py`](bridge/server.py). The device cushion fills from a 16-chunk (1 s) prebuffer; after that every chunk is sent at exactly `t0 + N * 60 ms` so the I2S queue never starves.

### `ocsc.v2` — the device wire protocol

| Frame | Direction | Payload |
|-------|-----------|---------|
| `hello` / `hello.ack` | both | Handshake + capability negotiation + boot counter for ghost-session detection. |
| `req` / `res` / `err` | both | RPC envelope. Methods include `say`, `face`, `motion.action`, `led.set`, `tts_stop`, `mic.start`, `cam.capture`, `show_text`, `play_pcm`. |
| `evt` | device → bridge | Async events: `head.tap`, `screen.tap`, `tts.done`, `mic.end`, `imu.shake`, `wake.fired`. |
| `mic.start` / `mic.end` | bridge → device | Bracket a mic streaming session. |
| `tts.start` / `tts.end` | bridge → device | Bracket a TTS stream. |
| `ping` / `pong` | both | 30 s heartbeat. Two misses → reconnect. |
| binary frame | both | 8-byte header (`version` + `kind` + `sid` + `seq`) + raw payload. Kinds: `MIC_PCM` (device→bridge), `TTS_PCM` (bridge→device), `CAM_JPEG`, `DISP_IMG`, `DISP_RGB565`. |

See [`bridge/protocol.py`](bridge/protocol.py) for the encoder/decoder.

## Firmware HTTP API (legacy / admin path)

The firmware also exposes a full HTTP+JSON API on port 80 — useful for admin work, demos, and pre-bridge testing. All endpoints accept an optional `X-StackChan-Token` header (set `STACKCHAN_TOKEN` in `config.h` to require auth).

- **Movement** — `/head`, `/head/normalized`, `/head/rotate`, `/head/stop`, `/head/torque`, `/head/calibrate`, `/home`.
- **Choreography** — `/action` (`home, nod, shake, yes, no, left, right, look_up, look_down, look_around, dance, surprised, sleep, wake, panic, peek, tilt_left, tilt_right, bow`).
- **Idle** — `/idle`, `/breathing`.
- **Display** — `/display`, `/display/big`, `/display/marquee`, `/display/progress`, `/display/brightness`, `/display/clear`, `/display/status`, `/display/face` (16 expressions: `neutral, happy, smile, love, sad, angry, surprised, thinking, sleep, wink_l, wink_r, stare, dead, embarrassed, cat, talking`).
- **LEDs** — `/led`, `/led/effect` (15 effects: rainbow, breathing, pulse, scanner, wipe, sparkle, police, fire, chase, theater, listening, thinking, talking, solid, off).
- **Audio** — `/speak {url}` (background WAV download + playback), `/mic/record {seconds, upload_url}` (record + auto-POST).
- **Sensors** — `/sensors` (IMU + magnetometer + touch + battery + camera), `/events` (ring buffer of physical events).
- **Camera** — `/camera/capture`, `/camera/config`, `/camera/preview`.
- **System** — `/status`, `/battery`, `/api`, `/volume`, `/reboot`.

`tools/openclaw_stackchan_demo.sh` exercises every one of these so you can confirm the firmware is healthy.

> **Note on `/camera/capture`**: there's a known bug where the `esp_camera` driver can lock up the device (requires physical power cycle to recover). The Hermes/OpenClaw skills gate this command behind a `--force` flag for safety.

## License

MIT. Doubao / Volcengine credentials are yours and never live in the repo; see `bridge/config.toml.example`.

## Acknowledgements

- The Mongonta [stack-chan](https://github.com/mongonta0716/stack-chan) repo for the mechanical platform and the inspiration.
- M5Stack for the CoreS3.
- ByteDance / Volcengine for the Doubao Flash ASR + unidirectional TTS endpoints — both substantially better than their bidirectional-streaming counterparts for short-turn voice loops.
- Wall‑E. Always Wall‑E.

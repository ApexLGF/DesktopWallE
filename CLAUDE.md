# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

ESP32 Arduino firmware for the **M5Stack CoreS3 + StackChan** robot. The firmware exposes a small HTTP+JSON API on the LAN so OpenClaw (an external agent host) can drive the screen, RGB LEDs, and the head servos. The device is the *server*; OpenClaw is the *client*. The firmware never calls OpenClaw — `OPENCLAW_GATEWAY_URL` is shown on the boot screen only.

## Build, flash, monitor

Before building, `include/config.h` must exist (it is gitignored). Create it from the example:

```sh
cp include/config.example.h include/config.h
```

**Arduino CLI is the primary toolchain** (PlatformIO is a fallback). The `StackChanOpenClaw/StackChanOpenClaw.ino` sketch is a one-line shim that `#include`s `../src/main.cpp` — Arduino CLI requires a sketch directory whose name matches the `.ino`, while PlatformIO uses `src/main.cpp` directly. Keep this shim if reorganizing.

```sh
arduino-cli compile --fqbn m5stack:esp32:m5stack_cores3 --build-path .arduino-build StackChanOpenClaw
arduino-cli upload  -p /dev/cu.usbmodem1301 --fqbn m5stack:esp32:m5stack_cores3 --input-dir .arduino-build StackChanOpenClaw
arduino-cli monitor -p /dev/cu.usbmodem1301 -c baudrate=115200
```

PlatformIO equivalent (uses `platformio.ini`, builds from `src/`):

```sh
pio run
pio run -t upload --upload-port /dev/cu.usbmodem1301
pio device monitor -p /dev/cu.usbmodem1301 -b 115200
```

There is no test suite, lint config, or CI in this repo. "Verification" means flashing the device and hitting the HTTP API (see `tools/openclaw_stackchan_demo.sh` for a smoke test).

## Architecture

Everything lives in `src/main.cpp` (single translation unit, anonymous namespace). The shape:

- `setup()` — `M5StackChan.begin()` → `Motion.goHome()` → set LEDs → `connectNetwork()` → `setupRoutes()` → `server.begin()` → draw status screen.
- `loop()` — pumps `M5StackChan.update()`, `server.handleClient()`, and the display auto-pager with a 2 ms delay. HTTP and motion handlers run cooperatively on this loop; long-running motion helpers (e.g. `actionNod`/`actionShake`) call `delay()` and block the web server. Audio playback is the exception — it runs on a dedicated FreeRTOS task (`speakTask`), so `POST /speak` returns immediately and audio does not block HTTP.
- `connectNetwork()` — tries STA mode using the configured SSID; on failure falls back to an open AP `StackChan-OpenClaw`. mDNS hostname is hardcoded to `stackchan` (so `http://stackchan.local`).
- HTTP layer — synchronous `WebServer` (port 80). Every handler runs `requireAuth()` first; if `STACKCHAN_TOKEN` is non-empty in `config.h`, the `X-StackChan-Token` header is required. CORS headers are added on every response via `sendCors()`.

### Servo angle convention (important — easy to get wrong)

`M5StackChan.Motion.move(x, y, speed)` uses the StackChan library's unit where **10 = 1 degree**. The firmware clamps:

- `x`: `[-1280, 1280]` (`kHeadXMin`/`kHeadXMax`)
- `y`: `[0, 850]` in code (`kHeadYMin`/`kHeadYMax`). The README recommends `y >= 50` to avoid servo end-stops — the code does **not** enforce that lower bound, callers should.
- `speed`: `[0, 1000]` (`kSpeedMin`/`kSpeedMax`)

State (`currentX`, `currentY`, `lastTitle`, `lastText`) is held in file-scope statics and reported by `GET /status`. There is no persistence across reboots.

### Predefined actions

`POST /action` accepts `name` ∈ `home | nod | shake | left | right | look_up | look_down`. New actions go in `handleAction()` alongside a helper like `actionNod()`. Anything that drives the LEDs across all 12 pixels should go through `setAllLeds()` (which calls `refreshRgb()` once) rather than looping over `setRgbColor` in the handler.

### Display rendering (Chinese + pagination)

Both `drawStatusScreen` and `drawMessage` use M5GFX's built-in efont fonts (`fonts::efontCN_24` for titles, `fonts::efontCN_16` for body) so UTF-8 CJK renders without any external font assets. Do **not** wrap `setTextSize(2)` around these — efontCN is a bitmap font, scaling blurs it; pick a bigger efont variant instead (efontCN_24 vs efontCN_16 vs efontCN_14, etc.).

Long `text` is auto-paginated: `splitTextIntoLines()` walks the UTF-8 bytes (`utf8CharBytes` handles 1/2/3/4-byte leads) and uses `display.textWidth()` to break lines at the current font's measured width. The pager state lives in `messageLines`/`currentPage`/`totalPages`/`lastPageMillis`; `loop()` advances pages every `kPageIntervalMs` (3.5 s) when `totalPages > 1` and draws a `current/total` indicator in the bottom-right. Single-page messages and the status screen leave `totalPages == 1` so they are never auto-redrawn.

### `/speak` background playback

`POST /speak {"url":"...wav","title":"..."}` enqueues onto `speakQueue` (FreeRTOS queue, depth 2) and returns `202 {"ok":true,"id":N}`. The dedicated `speakTask` (pinned to core 1, 8 KB stack) wakes, sets state via `speakSetState()` under `speakStateMutex`, opens `HTTPClient`, parses the WAV header chunks (`parseWavHeader` reads `RIFF`/`fmt `/`data`, validates PCM linear 16-bit mono-or-stereo), then **downloads the full PCM body into a heap buffer (DRAM preferred via `MALLOC_CAP_INTERNAL`, PSRAM fallback), then calls `M5.Speaker.playRaw()` once on the entire buffer**, and finally `free()`s after `isPlaying()` returns false. A new `/speak` request `M5.Speaker.stop()`s the current playback and drains the queue before enqueuing — there is no on-device queue of clips. `speakStopRequested` is `volatile bool`; the task checks it inside every read/loop boundary. Speaker DMA is configured with `dma_buf_count=8, dma_buf_len=1024` (~256 ms cushion at 16 kHz mono) in `setup()`.

**Why download-then-play instead of streaming**: early version did `read 4KB → playRaw 4KB` in a loop. Audio was choppy because the default Speaker DMA buffer (~24 ms) underran on TCP jitter. Downloading first eliminates network from the playback critical path. The 4 MB `kSpeakMaxBytes` ceiling caps DoS exposure; bump it if you ever need longer clips (1 minute @ 16 kHz mono ≈ 2 MB).

**Why `client->read(buf, n)` and not `readBytes`**: `Stream::readBytes` falls back to a per-byte `timedRead` loop in the base class, which made 175 KB take ~17 seconds. The `Client::read(uint8_t*, size_t)` virtual reads in TCP-segment-sized chunks (~1.4 KB) and gets the same payload in ~4 seconds. If you ever go back to `readBytes`, profile first.

Only **PCM WAV, 16-bit signed, mono or stereo, 8–48 kHz** is supported. No MP3/Opus/AAC/μ-law/float decoding — convert on the OpenClaw side. The contract is also documented in `openclaw-stackchan-skill/SKILL.md`; keep both in sync.

### Adding an endpoint

1. Add a `handleXxx()` in the anonymous namespace following the pattern: `requireAuth()` → `parseBody()` (if POST with JSON) → do work → `sendOk()` / `sendError()`.
2. Register it in `setupRoutes()`.
3. Mirror the contract in `openclaw-stackchan-skill/SKILL.md` so OpenClaw's agent knows about it. The skill is the public contract — keep it in sync.

## Configuration

`include/config.h` is gitignored and must define four macros (see `include/config.example.h`):

- `STACKCHAN_WIFI_SSID`, `STACKCHAN_WIFI_PASSWORD` — 2.4 GHz network. Empty SSID → AP fallback.
- `STACKCHAN_TOKEN` — empty disables auth; non-empty requires `X-StackChan-Token` header.
- `OPENCLAW_GATEWAY_URL` — display-only; firmware does not connect to it.

`main.cpp` uses `#if __has_include(...)` so the source still compiles without `config.h`, falling back to empty strings (device boots into AP mode, no auth).

## `openclaw-stackchan-skill/`

Not part of the firmware build. It is a Claude-Code-style skill (with `SKILL.md` and `agents/openai.yaml`) that gets installed into OpenClaw so its agent can call this device's HTTP API. Treat `SKILL.md` as the source of truth for what OpenClaw believes it can do — if the firmware API changes, this file must change with it.

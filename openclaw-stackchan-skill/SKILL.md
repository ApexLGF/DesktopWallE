---
name: stackchan
description: Control the M5 StackChan desktop robot — talks to OpenClaw via the openclaw-stackproxy-plugin (long-lived WebSocket through stackproxy daemon). Exposes 4 LLM tools — `stackchan_say`, `stackchan_express`, `stackchan_move`, `stackchan_look` — and a `stackproxy.invoke` gateway method for voice-driven dialog. Head motion, animated face, RGB LEDs, paged text, real-time Doubao TTS playback, real-time Doubao ASR (tap-to-talk), 0.3 MP camera capture, 9-axis IMU + compass. Use whenever the user asks OpenClaw to make StackChan move, show, speak, listen, look, or react.
---

# StackChan — OpenClaw desktop robot (v0.5)

StackChan is a small servo-driven robot head (M5Stack CoreS3 + StackChan kit). As of firmware v0.5.0 it speaks two languages with OpenClaw:

1. **The long-connection (preferred):** a persistent WebSocket from the device to the `stackproxy` daemon (`ocsc.v1` sub-protocol), with the `openclaw-stackproxy-plugin` bridging both sides into the OpenClaw agent runtime.
2. **The legacy HTTP API (still works):** the synchronous JSON-over-HTTP server on port 80, kept as admin/debug/fallback.

When an OpenClaw agent wants to drive StackChan it should **prefer the four registered tools** (`stackchan_say`, `stackchan_express`, `stackchan_move`, `stackchan_look`) rather than handcrafting `curl`. They go through `stackproxy → ws → device` and queue properly behind ongoing TTS / mic streams.

> ### ⚠️ Read this first — making StackChan speak
> **To make StackChan say something, call the `stackchan_say` tool with plain text. Nothing else.**
> You do **NOT** need: a WAV file, a TTS service, an audio URL, or any audio conversion. stackproxy already does Doubao TTS and streams the audio to the device for you.
> If you catch yourself reasoning *"I don't have a TTS service so I'll just show text on screen instead"* — that is wrong. `stackchan_say("窗前明月光")` makes the device speak it aloud. The same applies to expressions/motion: just call `stackchan_express` / `stackchan_move`.

## Target

- Plugin-route (preferred): the four tools registered by `openclaw-stackproxy-plugin`. Just call the tool — provider/auth/transport is handled.
- Stackproxy HTTP admin (debug only): `http://127.0.0.1:8766` on the OpenClaw host.
- Device HTTP (legacy / admin): `http://stackchan.local` (mDNS) or `http://<device-ip>` in this workspace.
- Voice loop: stackproxy listens on `ws://0.0.0.0:8765` and on `mic.end` POSTs the transcript to `openclaw gateway call stackproxy.invoke` → agent runs (with mem0 hooks + skills) → reply is TTS'd back to the device via streaming binary frames.

## When to use which tool (vs the legacy HTTP API)

| Goal | Use |
|------|-----|
| Speak text aloud through the device | `stackchan_say` |
| Change facial expression | `stackchan_express` |
| Run a predefined head motion | `stackchan_move` |
| Capture a still image | `stackchan_look` |
| Voice-trigger an agent turn from user speech | already wired — user taps StackChan's head, talks, taps again, agent runs, reply is spoken |
| Anything not covered above (LEDs, raw head x/y, marquee, sensors, camera config) | the legacy HTTP endpoints below |

## Auth

If `STACKCHAN_TOKEN` is non-empty in firmware config, include header `X-StackChan-Token: <token>` on every request. Otherwise unauthenticated. The plugin forwards this header if you set `stackchanToken` in `plugins.entries.openclaw-stackproxy-plugin.config`.

## Plugin tools (preferred)

### `stackchan_say`
Speak text aloud. Stackproxy synthesizes via Doubao TTS and streams binary PCM frames to the device for playback.

```jsonc
{ "text": "你好，我是 StackChan", "title": "OpenClaw" }
```

`text` is required (1-200 chars). `title` is optional (shows on the screen alongside the spoken text). `device` is optional (defaults to first connected device).

### `stackchan_express`
Switch facial expression. Choose one of: `neutral`, `happy`, `sad`, `angry`, `surprised`, `sleepy`, `loving`, `excited`.

```jsonc
{ "expression": "happy" }
```

### `stackchan_move`
Trigger a predefined head motion. Choose one of: `home`, `nod`, `shake`, `left`, `right`, `look_up`, `look_down`. (For fine-grained x/y control use the legacy `/head` HTTP endpoint.)

```jsonc
{ "name": "nod" }
```

### `stackchan_look`
Snap a still image from the front camera. Returns JPEG bytes (size summarized in the tool result). The image isn't auto-fed to a vision model — combine with an image-understanding tool if you want the agent to "see".

```jsonc
{}
```

## Voice loop (`stackproxy.invoke` gateway method)

When the user taps StackChan's head, the device starts streaming mic PCM over ws. Tapping again stops it. Stackproxy then:
1. Buffers all the PCM, posts it to Doubao v1 ASR (cluster `volcengine_streaming_common`), gets a final transcript.
2. Runs `openclaw gateway call stackproxy.invoke --params {"text":"...","deviceId":"...","boot_count":N}`.
3. The plugin builds a per-device session (`sessionKey: stackchan:<deviceId>:b<boot_count>`) and calls `api.runtime.agent.runEmbeddedPiAgent` so mem0 hooks fire and the agent has access to every tool — including the four StackChan ones.
4. The agent's text reply is synthesized via Doubao TTS and streamed back as binary PCM frames; the device plays it via `M5.Speaker.playRaw`.

You normally don't call `stackproxy.invoke` directly — the user does, by talking to the device. But it's available from the CLI for tests:

```bash
openclaw gateway call stackproxy.invoke \
  --params '{"text":"hi","deviceId":"stackchan-<mac>","boot_count":1}' \
  --json --timeout 60000
```

## What StackChan can do

| Capability       | Endpoints |
|------------------|-----------|
| Move head        | `/head`, `/head/normalized`, `/head/rotate`, `/head/stop`, `/head/torque`, `/head/calibrate`, `/home`, `/action` |
| Predefined moves | `/action` (`name`: home, nod, shake, yes, no, left, right, look_up, look_down, look_around, dance, surprised, sleep, wake, panic, peek, tilt_left, tilt_right, bow) |
| Idle behavior    | `/idle` (random head sway), `/breathing` (subtle nod oscillation) |
| Show text        | `/display`, `/display/clear`, `/display/big`, `/display/marquee`, `/display/progress`, `/display/brightness`, `/display/status` |
| Animated face    | `/display/face` (neutral, happy, smile, love, sad, angry, surprised, thinking, sleep, wink_l, wink_r, stare, dead, embarrassed, cat, talking) — auto-blinks |
| Color LEDs       | `/led` (solid), `/led/effect` (rainbow, breathing, pulse, scanner, wipe, sparkle, police, fire, chase, theater, listening, thinking, talking, recording), `/led/off` |
| **Speak (TTS)**  | **Use the `stackchan_say` tool — pass plain text, that's it.** stackproxy synthesises it with Doubao TTS and streams the audio to the device. You do NOT need a WAV file, a TTS service, or an audio URL. (The legacy `/speak {url}` HTTP endpoint still exists but requires a pre-rendered WAV — ignore it; `stackchan_say` replaces it entirely.) |
| Listen (STT)     | Voice input is already wired: the user taps StackChan's head, talks, taps again — stackproxy runs Doubao ASR and feeds the transcript to you. You don't drive listening; you just receive the transcript. (Legacy `/mic/record` HTTP endpoint exists for admin/debug only.) |
| See (camera)     | `/camera/capture` (returns JPEG image; software-encoded from RGB565 because the GC0308 has no hardware JPEG), `/camera/config` (resolution, quality, mirror/flip, brightness/contrast/saturation/effect), `/camera/preview` (push the live frame to the LCD), `/camera/init`, `/camera/deinit`, `/camera/status` |
| Sense the world  | `/sensors` (IMU accel + gyro + magnetometer + compass heading, head capacitive touch zones, screen touch, battery, head pose), `/events` (queued events since last id), `/webhook` (POST events to your URL) |
| System           | `/status`, `/battery`, `/api`, `/volume`, `/reboot` |

## Endpoint reference

### `GET /status`
Returns a rich snapshot: firmware version, host, IP, RSSI, current head x/y, display mode + last text/title + current face, LED effect + RGB + speed, idle/breathing toggles, configured webhook, speak status.

### `GET /api`
Returns `{ok, firmware, endpoints[]}` — useful for discovery.

### `GET /battery`
`{ok, voltage, current, level, charging}`. `current` is positive when discharging, negative when charging.

### `GET /sensors`
Live snapshot of every onboard sensor:
```json
{
  "imu": {
    "accel": {"x":..,"y":..,"z":..},
    "gyro":  {"x":..,"y":..,"z":..},
    "mag":   {"x":..,"y":..,"z":..},   // BMM150 magnetometer (μT)
    "heading_deg": 183.0                // compass heading from raw mag
  },
  "camera": {"enabled":bool,"frame_size":"qvga","quality":int,"last_bytes":int},
  "head_touch": {"front":0-3,"middle":0-3,"back":0-3},
  "screen_touch": {"touched":bool,"x":int,"y":int},
  "battery": {"voltage":V,"level":0-100,"charging":int},
  "head": {"x":..,"y":..,"moving":bool}
}
```

`heading_deg` is raw uncompensated magnetometer atan2; it tracks rotation reliably even though it isn't tilt-compensated. Pair with `accel` for level-corrected heading if you need higher accuracy.

### `GET /events?since=<id>`
Returns queued events with monotonically increasing ids (ring buffer of 64). Poll with the latest `id` you have to get only new events. Event types currently emitted:

- `boot` — firmware version in `data`
- `head.tap` — head capacitive sensor was clicked; `data` lists last intensities
- `head.swipe` — `data=dir=forward` or `dir=backward`
- `screen.tap` — capacitive screen tap; `data=x=.. y=..`
- `btn.a` / `btn.b` / `btn.c` / `btn.pwr` — physical/virtual buttons
- `imu.shake` — shake gesture above threshold
- `imu.face_down` — unit was turned upside down
- `speak.start` / `speak.done` / `speak.stopped` / `speak.error`
- `mic.start` / `mic.done` / `mic.error`

If a webhook is configured (see `/webhook`), every event is also POSTed there.

### `POST /display` `{title, text}`
UTF-8 (CJK + emoji-free symbols) text. Long body auto-paginates ~10 lines/page, cycling every 3.5s with a `current/total` indicator. Title uses efontCN_24, body uses efontCN_16. Send the whole message in one call — don't loop to "scroll".

### `POST /display/clear`
Black screen.

### `POST /display/face` `{expression, eye_color?, mouth_color?, bg?}`
Renders a programmatic avatar face with two eyes and a mouth. Colors are 6-digit hex strings (no `#`). The face auto-blinks every 3-6s.

Expressions: `neutral`, `happy`, `smile`, `love` (with floating heart), `sad`, `angry`, `surprised`, `thinking` (eyes look up-right), `sleep` (closed eyes + Zz), `wink_l`, `wink_r`, `stare`, `dead`, `embarrassed` (with sweat drop), `cat` (`>3<` mouth), `talking`.

### `POST /display/big` `{text, color?}`
Single large centered string (efontCN_24 × 2). Use for short notifications (timer, "Done!", a number).

### `POST /display/marquee` `{text, step_ms?, color?}`
Right-to-left scrolling text, looped. `step_ms` is per-pixel cadence (default 40).

### `POST /display/progress` `{percent, label?}`
Progress bar with percent readout.

### `POST /display/brightness` `{value}` (0–255)
Backlight brightness.

### `POST /display/status`
Repaint the boot status screen (Wi-Fi / IP / firmware).

### `POST /led` `{r, g, b}` or `{r, g, b, index}`
Solid color on all 12 LEDs, or one individual LED (index 0–11; 0–5 left, 6–11 right).

### `POST /led/effect` `{name, r?, g?, b?, speed_ms?}`
Run a non-blocking effect. `speed_ms` controls per-frame delay (default 80). Effects:

| Effect      | Notes |
|-------------|-------|
| `off`       | All off |
| `solid`     | Same as `/led` |
| `rainbow`   | HSV wheel, ignores `r/g/b` |
| `breathing` | Sinusoidal fade of `r/g/b` |
| `pulse`     | Triangle-wave fade |
| `scanner`   | KITT-style sweep |
| `wipe`      | Linear fill |
| `sparkle`   | Random twinkle |
| `police`    | Alternating red/blue halves |
| `fire`      | Orange/red flicker |
| `chase`     | Single dot rotates with fading tail |
| `theater`   | 3-step marquee |
| `listening` | Soft cyan breathing |
| `thinking`  | Purple rotating dot (call when LLM is processing) |
| `talking`   | Green pulse (call while speech is playing) |
| `recording` | Hard red blink at ~2.5 Hz — unmistakable "mic is live" indicator (v0.5) |

### `POST /led/off`
Shortcut for effect=off.

### `POST /head` `{x, y, speed}`
Raw servo target. Units: yaw `x` in `[-1280, 1280]`, pitch `y` in `[0, 850]` (recommend `y >= 50`), `speed` in `[0, 1000]`. Internally **10 units = 1 degree** per the StackChan motion library.

### `POST /head/normalized` `{x, y, speed}`
Normalized targeting in `[-1.0, 1.0]` on both axes. Use for face-tracking (e.g. center on a detected face).

### `POST /head/rotate` `{velocity}`
Continuous yaw rotation. `velocity ∈ [-1000, 1000]`; negative = clockwise, positive = counter-clockwise. Stop with `POST /head/stop`.

### `POST /head/stop`
Halt any in-flight motion (including continuous rotation).

### `POST /head/torque` `{enabled}`
Toggle servo torque. Disable when the user wants to pose the head by hand; re-enable to resume control.

### `POST /head/calibrate`
Take the current physical pose as the new home (0,0). Run after re-mounting the head.

### `POST /home`
`Motion.goHome()` — return to (0,0).

### `POST /action` `{name}`
Run a named choreography. See the table above.

### `POST /idle` `{enabled}`
When `enabled:true`, the firmware moves the head to a random small target every 4–9s. Toggling on makes StackChan feel alive between explicit commands. Disable before any motion-precise demo.

### `POST /breathing` `{enabled}`
When `enabled:true`, applies a subtle ±20-unit pitch oscillation around the current Y (look like the robot is breathing). Independent of `/idle`.

### `POST /speak` `{url, title?}` — ⚠️ LEGACY, DO NOT USE

**To make StackChan speak, use the `stackchan_say` tool instead — pass plain text and you're done.** That tool handles TTS synthesis (Doubao) and audio streaming automatically.

This `/speak` endpoint is the old v0.4 path: it needs a **pre-rendered WAV file** served over HTTP (PCM 16-bit, 8–48 kHz, no MP3/Opus). It exists only for admin/debug and is fully superseded by `stackchan_say`. If you ever find yourself thinking "I need a TTS service to generate a WAV" — stop, you don't; just call `stackchan_say` with text.

### `GET /speak/status`
`{ok, playing, id, url, title}`. Poll if you need to chain clips.

### `POST /speak/stop`
Cut current playback. Idempotent.

### `POST /mic/record` `{seconds, sample_rate?, upload_url?, prompt?}`
Record up to 10 seconds of 16-bit mono audio. If `upload_url` is set, the firmware HTTP-POSTs the WAV body (`Content-Type: audio/wav`) to that URL after recording. If `prompt` is set, the firmware sends it as `X-StackChan-Prompt`. Returns `202 {ok, id, seconds, upload}`.

**Auto-speak loop**: if the upload endpoint returns JSON `{speak_url:"...", title?:"..."}`, the firmware immediately enqueues `/speak` with that URL. This gives you a one-shot conversation cycle: **tap head → record → upload to OpenClaw → response WAV → play back**.

### `GET /mic/status`
`{ok, recording, id, last_bytes, last_note}`.

### `GET /mic/last`
Re-download the most recent recording as `audio/wav`. Useful if your upload endpoint failed.

### `POST /mic/stop`
Abort an in-flight recording.

### `POST /camera/init` `{frame_size?, quality?}`
Initialise the GC0308 camera (lazy — the firmware also initialises it on the first `/camera/capture` if it hasn't been booted yet). Frame size is one of `96x96 | qqvga | qcif | hqvga | 240x240 | qvga | cif | hvga | vga` (default `qvga` = 320×240). `quality` is 0–63 (lower = better; default 12).

### `GET /camera/capture`
Returns `image/jpeg`. Default 320×240 ≈ 4–10 KB, ~1–7 s round-trip. VGA 640×480 ≈ 60–90 KB, ~5–8 s round-trip. **Always prefer QVGA** unless you specifically need detail — the firmware software-encodes JPEG (the GC0308 has no hardware JPEG), so VGA is much slower and chews TCP bandwidth.

Hand the returned bytes straight to a vision model (OpenAI `gpt-4o-mini` with `detail: "low"` works great at QVGA).

**Important — camera is the only flaky path in v0.4.0.** Empirical behaviour:

- **First capture after boot** succeeds reliably (~1–3 s).
- **Back-to-back captures**: ~80% complete (most in 1–7 s). Some get truncated mid-stream, and **occasionally** the device's WebServer goes unresponsive for ~20–60 s afterward.
- The firmware **auto-deinits the camera after every shot** so PSRAM is freed and the GC0308 background task doesn't fight the M5 I2C bus. Subsequent captures pay a ~250 ms init cost.

**Recommended usage:**

1. **Leave 5–10 seconds between captures.** Don't burst-shoot.
2. **One capture per agent turn is usually enough** — most vision tasks (describe scene, read text, find object) need a single frame.
3. **If the next request after a capture times out**, give the device 30 s to self-recover before retrying. As a last resort `POST /reboot`.
4. **The other 35+ endpoints stay fully responsive** while the camera is idle — only the immediate post-capture window is degraded.

### `POST /camera/config` `{frame_size?, quality?, brightness?, contrast?, saturation?, effect?, hmirror?, vflip?, gainceiling?, awb?, aec?}`
Live tweak the sensor:
- `frame_size`: as above
- `quality`: 0–63
- `brightness/contrast/saturation`: -2..+2
- `effect`: 0=none, 1=negative, 2=B&W, 3=red, 4=green, 5=blue, 6=retro
- `hmirror/vflip`: bool, useful when the StackChan is mounted upside-down
- `gainceiling`: 0..6 (raises ISO ceiling in dim light, adds noise)
- `awb/aec`: auto white balance / auto exposure (bool, default on)

### `POST /camera/preview`
Push one frame to the LCD instead of returning it. Useful for "viewfinder" demos — the screen briefly shows what the camera is seeing.

### `POST /camera/deinit`
Release the camera framebuffers (~614 KB of PSRAM at QVGA, ~614 KB still — same size; actually 320×240×2 = 153 KB; VGA = 614 KB). Call before any operation that needs the PSRAM elsewhere.

### `GET /camera/status`
`{ok, enabled, init_tried, frame_size, quality, last_capture_ms, last_bytes, error?}`.

### `POST /webhook` `{url}`
Set a URL that StackChan should POST every event to. Empty URL clears it. Payload:
```json
{"id":42, "ts":<uptime_ms>, "type":"head.tap", "data":"intensities=0,2,0"}
```
Use this to push-notify OpenClaw of physical interactions instead of polling `/events`.

### `GET /webhook`
Read the current webhook URL.

### `POST /volume` `{value}` (0–255)
Speaker volume.

### `POST /reboot`
Soft reboot the firmware.

## Recommended patterns

### Greet on boot
```bash
B=http://stackchan.local
curl --noproxy '*' -sS -X POST $B/display/face -H 'Content-Type: application/json' -d '{"expression":"happy"}'
curl --noproxy '*' -sS -X POST $B/led/effect  -H 'Content-Type: application/json' -d '{"name":"breathing","r":0,"g":120,"b":255,"speed_ms":40}'
curl --noproxy '*' -sS -X POST $B/idle        -H 'Content-Type: application/json' -d '{"enabled":true}'
```

### Pop a notification
```bash
curl --noproxy '*' -sS -X POST $B/display/big -H 'Content-Type: application/json' \
  -d '{"text":"✓ 完成","color":"66FF99"}'
curl --noproxy '*' -sS -X POST $B/action -H 'Content-Type: application/json' -d '{"name":"yes"}'
```

### Long message that paginates
```bash
curl --noproxy '*' -sS -X POST $B/display -H 'Content-Type: application/json' -d '{
  "title":"今日天气",
  "text":"上午多云，气温 18°C。\n下午转晴，最高 24°C。\n夜间有阵雨，注意带伞。"
}'
```

### TTS pipeline (OpenClaw side)
On macOS the `say` + `afconvert` chain is the simplest test fixture. In production, prefer the `sherpa-onnx-tts` OpenClaw skill (offline, no cloud) or `openai-whisper-api`/`elevenlabs` for higher quality.

```bash
# 1. Synthesize
say -v Tingting -o /tmp/clip.aiff "你好，我是 StackChan。"
afconvert -f WAVE -d LEI16@16000 -c 1 /tmp/clip.aiff /tmp/clip.wav

# 2. Host on a URL StackChan can reach (your existing OpenClaw box)
#    e.g. /tmp/clip.wav -> http://<openclaw-host>/tts/clip.wav

# 3. Tell StackChan to play it
curl --noproxy '*' -sS -X POST $B/speak -H 'Content-Type: application/json' \
  -d '{"url":"http://<openclaw-host>/tts/clip.wav","title":"打招呼"}'

# 4. (optional) Mirror the playback in body language
curl --noproxy '*' -sS -X POST $B/led/effect -H 'Content-Type: application/json' \
  -d '{"name":"talking"}'
```

### Conversation loop (push-to-talk via head tap)
Wire a webhook from StackChan to OpenClaw, then drive a record→transcribe→reply→speak cycle:

```bash
# Tell StackChan to push events to OpenClaw
curl --noproxy '*' -sS -X POST $B/webhook -H 'Content-Type: application/json' \
  -d '{"url":"http://openclaw.lan/stackchan/event"}'
```

When `head.tap` fires on the webhook, OpenClaw replies by starting the record cycle:

```bash
# Tell StackChan to listen for 4s and upload to your STT endpoint
curl --noproxy '*' -sS -X POST $B/mic/record -H 'Content-Type: application/json' \
  -d '{"seconds":4,"sample_rate":16000,"upload_url":"http://openclaw.lan/stackchan/listen"}'
```

The `/stackchan/listen` handler on OpenClaw receives `audio/wav`, runs Whisper/Sherpa, prompts an LLM, runs TTS, hosts the result, and replies JSON `{speak_url:"http://...wav","title":"reply"}`. StackChan auto-plays it.

### "Listening" body language
When you start recording, drive the LEDs and face to match. The user sees that the robot is paying attention:

```bash
curl --noproxy '*' -sS -X POST $B/led/effect    -d '{"name":"listening"}' -H 'Content-Type: application/json'
curl --noproxy '*' -sS -X POST $B/display/face  -d '{"expression":"stare"}' -H 'Content-Type: application/json'
curl --noproxy '*' -sS -X POST $B/mic/record    -d '{"seconds":4,"upload_url":"http://openclaw.lan/stackchan/listen"}' -H 'Content-Type: application/json'
```

While LLM is thinking:
```bash
curl --noproxy '*' -sS -X POST $B/led/effect   -d '{"name":"thinking"}' -H 'Content-Type: application/json'
curl --noproxy '*' -sS -X POST $B/display/face -d '{"expression":"thinking"}' -H 'Content-Type: application/json'
```

While speaking:
```bash
curl --noproxy '*' -sS -X POST $B/led/effect   -d '{"name":"talking"}' -H 'Content-Type: application/json'
curl --noproxy '*' -sS -X POST $B/display/face -d '{"expression":"talking"}' -H 'Content-Type: application/json'
```

### Vision: "look and describe"
Fetch a JPEG with `/camera/capture`, post it to a vision-LLM, speak or display the result. The bundled `tools/openclaw_bridge.py` has a `/stackchan/see` endpoint that does exactly this — send it `{prompt, speak}` and it captures, runs `gpt-4o-mini` vision, then speaks or displays the answer.

```bash
# raw — fetch the camera frame yourself
curl --noproxy '*' http://stackchan.local/camera/capture --output /tmp/look.jpg

# through the bridge — one call, end-to-end
curl --noproxy '*' -X POST http://openclaw.lan:18790/stackchan/see \
  -H 'Content-Type: application/json' \
  -d '{"prompt":"用一句中文说桌上有什么","speak":true}'
```

Best practice: set face + LED to "thinking" while the vision model is running, then "talking"/"happy" when the answer comes back.

### Face tracking (when a camera somewhere reports a normalized position)
```bash
# Track a target normalized to (-1..1, -1..1)
curl --noproxy '*' -sS -X POST $B/head/normalized -H 'Content-Type: application/json' \
  -d '{"x":0.2,"y":0.4,"speed":600}'
```

### Notify by physical posture
Combine motion, face, and LED into a single "look".

| Mood     | LED effect    | Expression  | Motion       |
|----------|---------------|-------------|--------------|
| Excited  | `rainbow`     | `happy`     | `dance`      |
| Sleepy   | `breathing`   | `sleep`     | `sleep`      |
| Confused | `pulse`       | `thinking`  | `tilt_left`  |
| Alert    | `police`      | `surprised` | `look_up`    |
| Loving   | `breathing`   | `love`      | `nod`        |
| Annoyed  | `fire`        | `angry`     | `shake`      |

## Output style for the agent

Keep responses short and action-first. Run multiple control endpoints in parallel where the network allows — the firmware is non-blocking for LEDs, idle motion, marquee, and speak/mic background tasks, so you can issue `/led/effect`, `/display/face`, and `/action` back-to-back without waiting for each to "finish". If a call fails, report the exact endpoint and the error body before trying another action.

When the user gives an open-ended request ("react to this"), feel free to compose a mood: pick a face, an LED effect, and a small motion — all three at once. That's what makes StackChan feel alive.

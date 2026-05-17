---
name: hotdog
description: "热狗 (Hotdog) — the M5 StackChan desk robot. Speaks (Doubao TTS via Bridge `/say`), nods/turns its head, blinks its 12-LED neon ring, reports its sensors. Control is via the Bridge daemon on the Mac mini at 127.0.0.1:8766 — call `/say` for speech, `/rpc` for everything else with the EXACT method names listed below."
version: 2.1.0
author: Hermes Agent
license: MIT
platforms: [macos, linux]
metadata:
  hermes:
    tags: [Robot, Hardware, IoT, Voice, TTS, ASR, M5Stack, Desk-Toy, Smart-Home, MCP]
prerequisites:
  commands: [curl]
---

# 热狗 (Hotdog) — desk robot

A small M5Stack CoreS3 + StackChan robot head on the desk. You drive it through the
**Bridge daemon** on the Mac mini (`http://127.0.0.1:8766`). The Bridge owns the long
WebSocket connection to the device and translates HTTP into the on-wire RPC.

## ⚠️ Read this before guessing method names

The Bridge `/rpc` endpoint is a *generic dispatcher*. The device firmware implements
**exactly these methods, and nothing else**:

| Method | Params | What it does |
|---|---|---|
| `ping` | none | Liveness check. |
| `move` (alias: `move_head`, `head`) | `{x, y, speed}` | Move head to absolute servo position. `x` -1280..1280 (yaw, −=left), `y` 30..850 (pitch, small=up, ~425=neutral), `speed` 0..1000. |
| `head_normalized` | `{nx, ny, speed}` | Same motion in normalized [-1, 1] coordinates (used by face tracker). |
| `action` (alias: `do_action`) | `{name}` | Predefined gesture. `name` ∈ `home`, `nod`, `shake`, `yes`, `no`, `look_up`, `look_down`, `look_left`, `look_right`, `look_around`, `dance`, `surprised`, `sleep`, `wake`, `panic`, `peek`, `bow`, `tilt_left`, `tilt_right`. Blocks the call until the gesture finishes (≤3 s). |
| `torque` | `{enabled: bool}` | Enable / disable holding torque on both head servos. Disabled = head goes limp (good for being moved by hand). |
| `set_led` | `{r, g, b}` or `{r, g, b, index}` | Solid color on the 12-LED ring. Without `index`: all LEDs. With `index` 0..11: just that one. Each channel 0..255. |
| `set_led_effect` (alias: `led_effect`) | `{name, r?, g?, b?, speed_ms?}` | Animated effect. `name` ∈ `off`, `solid`, `rainbow`, `breathing`, `pulse`, `scanner`, `wipe`, `sparkle`, `police`, `fire`, `chase`, `theater`, `listening`, `thinking`, `talking`, `recording`. `r/g/b` is the base color where applicable; `speed_ms` overrides the effect's default tick. |
| `face` (alias: `set_face`, `display_face`) | `{expression, eye_rgb?, mouth_rgb?, bg_rgb?}` | StackChan-style face on the LCD. `expression` ∈ `neutral`, `happy`, `smile`, `love`, `sad`, `angry`, `surprised`, `thinking`, `sleep`, `wink_l`, `wink_r`, `stare`, `dead`, `embarrassed`, `cat`, `speak`/`talking`. Colors are 24-bit packed (e.g. `0x00ff00`). |
| `show_text` | `{title, text}` | Title-driven LCD state hint (HEARD / THINK / TALK / ASR / IDLE …). For real text rendering, use Bridge `/show_text` (PIL on the Mac mini, ships JPEG). |
| `volume` | `{pct?}` (0..100) | Speaker volume. Without params → returns current. With `pct` → sets. Returns `{pct}`. |
| `brightness` | `{pct?}` (0..100) | LCD backlight via AXP2101 DLDO1. Same read/write pattern as `volume`. |
| `battery` | none | Returns `{voltage_mv, vbus_mv, percent, charging}` from the AXP2101 fuel gauge. |
| `status` (alias: `get_sensors`) | none | Returns `{uptime_ms, free_heap, free_psram, fw, app, vol, brightness, vbat_mv, charging}`. |
| `reboot` | none | Sends the ack frame then `esp_restart()` — bridge reconnects on the new boot. |
| `cam_capture` | internal | Don't call directly — Bridge wraps this in `/capture`. Currently disabled (esp_camera lockup on this firmware revision). |

**There is no `motion.nod`. There is no `led.set`. There is no `face.show`.** If you
call something not in the table above, the device returns `not_implemented` and `/rpc`
returns 502. The `motion` / `led` / `face` strings in the device's hello `caps[]` are
*capability tags*, NOT method names.

Speech, photos, image display, and face tracking are NOT `/rpc` methods — each has
its own dedicated Bridge endpoint (see below) because they need TTS / image processing /
binary transport that JSON RPC can't carry cleanly.

## The 4 things you actually call

### 1) Say something — `POST /say`

```sh
curl -s --noproxy '*' -X POST http://127.0.0.1:8766/say \
  -H 'Content-Type: application/json' \
  -d '{"text":"你好,我是热狗"}'
```

Returns `{"ok":true,"bytes":...,"est_ms":...}`. Audio plays for `est_ms` ms after the
call returns. For longer text expect ~30 chars/second of playback.

### 2) Predefined gesture — `POST /rpc` with `action`

```sh
curl -s --noproxy '*' -X POST http://127.0.0.1:8766/rpc \
  -H 'Content-Type: application/json' \
  -d '{"method":"action","params":{"name":"nod"},"timeout":8}'
```

`name` ∈ `home | nod | shake | yes | no | look_up | look_down | look_left | look_right | look_around | dance | surprised | sleep | wake | panic | peek | bow | tilt_left | tilt_right`. Quick gestures (nod / shake / yes / no) take ~1–2 s; `dance` and `look_around` take 3–5 s. The call blocks until done.

### 3) Set the LED ring — `POST /rpc` with `set_led`

```sh
# all 12 LEDs red
curl -s --noproxy '*' -X POST http://127.0.0.1:8766/rpc \
  -H 'Content-Type: application/json' \
  -d '{"method":"set_led","params":{"r":255,"g":0,"b":0}}'

# one LED at index 3 to green
curl -s --noproxy '*' -X POST http://127.0.0.1:8766/rpc \
  -H 'Content-Type: application/json' \
  -d '{"method":"set_led","params":{"r":0,"g":255,"b":0,"index":3}}'

# animated effect — rainbow / breathing / fire / police / scanner / etc.
curl -s --noproxy '*' -X POST http://127.0.0.1:8766/rpc \
  -H 'Content-Type: application/json' \
  -d '{"method":"set_led_effect","params":{"name":"breathing","r":40,"g":120,"b":200}}'
```

Effect names: `off`, `solid`, `rainbow`, `breathing`, `pulse`, `scanner`,
`wipe`, `sparkle`, `police`, `fire`, `chase`, `theater`, `listening`,
`thinking`, `talking`, `recording`. The animation runs on the device until
you change effect or call `set_led_effect {name:"off"}`.

### 4) Take a photo — `POST /capture`

```sh
# raw JPEG body (320x240, ~20-30 KB at default quality)
curl -s --noproxy '*' -X POST http://127.0.0.1:8766/capture \
  --max-time 15 -d '{}' -o /tmp/hotdog.jpg

# or JSON-wrapped with base64 payload (for embedding in chat replies)
curl -s --noproxy '*' -X POST 'http://127.0.0.1:8766/capture?format=json' \
  --max-time 15 -d '{}' | jq '{ok, w, h, bytes, jpeg_b64: (.jpeg_b64 | .[0:60] + "…")}'
```

Use this with `find_face` for the "take a photo of me" compound flow.

### 5) Show an image on the LCD — `POST /show_image`

Three ways to pass the image. Use whichever is easiest:

```sh
# (a) URL — bridge downloads it for you (recommended for web images)
curl -s --noproxy '*' -X POST \
  'http://127.0.0.1:8766/show_image?url=https://example.com/cat.jpg'

# (b) Local file path — bridge reads it directly (recommended for files
#     on the same Mac mini, e.g. a chart you just rendered with matplotlib)
curl -s --noproxy '*' -X POST \
  'http://127.0.0.1:8766/show_image?path=/tmp/chart.png'

# (c) Raw bytes — pipe the image data in
curl -s --noproxy '*' -X POST http://127.0.0.1:8766/show_image \
  -H 'Content-Type: image/png' --data-binary @some-pic.png
```

Bridge accepts any format Pillow can decode (JPEG / PNG / WebP / GIF / BMP),
auto-resizes + letterboxes onto a black 320×240 canvas, re-encodes JPEG q=80,
and pushes to the device. Returns once bytes are queued — fire-and-forget.

Good for: a chart you just generated, a meme / sticker, a screenshot of a
search result, a QR code, the output of an image-generation model.

### 6) Find / track a face — `POST /find_face`, `POST /track`

```sh
# one-shot: sweep the head, lock onto the largest detected face
curl -s --noproxy '*' -X POST http://127.0.0.1:8766/find_face --max-time 60 -d '{}'

# continuous tracking (bridge-side, ~2.5 fps OpenCV haar cascade)
curl -s --noproxy '*' -X POST http://127.0.0.1:8766/track -d '{"action":"start"}'
curl -s --noproxy '*' -X POST http://127.0.0.1:8766/track -d '{"action":"stop"}'
curl -s --noproxy '*' -X POST http://127.0.0.1:8766/track -d '{"action":"status"}'
```

`find_face` returns `{locked: true, x, y, face:{cx,cy,w,h}}` on success.

### 7) Show a robot face — `POST /face`

热狗 has a 320×240 LCD. By default the bridge shows a built-in robot face
(eyes + mouth, cyan on dark navy). After every connect, every `/perform`,
and on demand via this endpoint, the bridge re-renders a face on the host
with PIL and ships it as one JPEG.

```sh
curl -s --noproxy '*' -X POST http://127.0.0.1:8766/face \
  -d '{"expr":"happy"}'
```

`expr` ∈ `idle` (default sleepy smile) | `happy` | `sad` | `surprised` |
`talking` | `listening` | `sleepy` | `blink` | `thinking` | `alert`.

Use this for **standalone mood changes** (e.g. acknowledging the user,
showing surprise on an alert). For per-line subtitles **during** a
recitation, attach text/face fields to the `/perform` cues — don't
call `/face` from a loop.

### 8) Show text on the LCD — `POST /show_text`

```sh
# big-text on a card
curl -s --noproxy '*' -X POST http://127.0.0.1:8766/show_text \
  -d '{"text":"你好，我是热狗", "title":"打招呼", "face":"happy"}'

# multi-line announcement, large font
curl -s --noproxy '*' -X POST http://127.0.0.1:8766/show_text \
  -d '{"text":"提醒\n下午 3 点开会","size":"large"}'
```

Bridge renders with PIL (PingFang CJK font on macOS — CJK + ASCII fully
supported). Word-wraps automatically. Fields:

| Field | What |
|---|---|
| `text` | required, body string, `\n` for explicit line break |
| `title` | optional small heading above the body |
| `face` | optional inset face icon (`happy`/etc) in upper-left |
| `bg`, `fg` | optional `[r,g,b]` color overrides |
| `size` | `small`/`medium`/`large`/`auto` (default `auto`) |

Returns `{ok, bytes}`. Image is fire-and-forget — call returns once
the JPEG hits the WS, display update is ~10-20 ms after that.

### 9) `/perform` cue `text` field — auto-subtitles during recitation

Each cue in a `/perform` can carry a `text` field. The bridge will render
that string as a subtitle on the LCD at the cue's `at_ms` mark, in sync
with the audio. Pattern:

```sh
curl -X POST http://127.0.0.1:8766/perform -d '{
  "text": "(full poem joined)",
  "cues": [
    {"at_ms": 0,    "led": {...}, "motion":"look_up",   "text":"君不见黄河之水天上来"},
    {"at_ms": 4400, "led": {...}, "motion":"look_down", "text":"君不见高堂明镜悲白发"}
  ]
}'
```

After the last cue + ~1.5 s, bridge **automatically** restores the `idle`
face — don't add a "reset face" cleanup step.

### 10) Read sensors / status — `POST /rpc` with `status` or `battery`

```sh
# everything in one shot (uptime, free heap/psram, fw, vol, brightness, vbat)
curl -s --noproxy '*' -X POST http://127.0.0.1:8766/rpc \
  -H 'Content-Type: application/json' \
  -d '{"method":"status"}'

# just the battery
curl -s --noproxy '*' -X POST http://127.0.0.1:8766/rpc \
  -d '{"method":"battery"}'
```

`status` returns `{uptime_ms, free_heap, free_psram, fw, app, vol, brightness, vbat_mv, charging}`.
`battery` returns `{voltage_mv, vbus_mv, percent, charging}`.

### 11) Volume / brightness / reboot — `POST /rpc`

```sh
# set speaker volume to 60%
curl -s --noproxy '*' -X POST http://127.0.0.1:8766/rpc \
  -d '{"method":"volume","params":{"pct":60}}'

# read current volume (no params)
curl -s --noproxy '*' -X POST http://127.0.0.1:8766/rpc \
  -d '{"method":"volume"}'

# dim the LCD backlight
curl -s --noproxy '*' -X POST http://127.0.0.1:8766/rpc \
  -d '{"method":"brightness","params":{"pct":30}}'

# soft reboot the device
curl -s --noproxy '*' -X POST http://127.0.0.1:8766/rpc \
  -d '{"method":"reboot"}'
```

The device ACKs `reboot` then `esp_restart()`s; the bridge sees the connection
drop and the next `/status` lists `last_seen_age_s > 5` for ~3 s until the
device reconnects with `boot_count + 1`.

### 12) On-device face — `POST /rpc` with `face`

A second face renderer lives **on the device** (drawn straight to the LCD
framebuffer, no JPEG round-trip). It's faster and supports more expressions
than the bridge `/face` endpoint:

```sh
curl -s --noproxy '*' -X POST http://127.0.0.1:8766/rpc \
  -d '{"method":"face","params":{"expression":"happy","eye_rgb":"0x00ff00","mouth_rgb":"0x00ff00","bg_rgb":"0x000000"}}'
```

`expression` ∈ `neutral`, `happy`, `smile`, `love`, `sad`, `angry`,
`surprised`, `thinking`, `sleep`, `wink_l`, `wink_r`, `stare`, `dead`,
`embarrassed`, `cat`, `speak` / `talking`. Colors are 24-bit hex (string
`"0xRRGGBB"`) or 32-bit ints.

Use bridge `/face` (section 7) for the cyan-on-navy default robot look used
during recitation. Use this device `face` RPC when you want a vivid mood
expression that pops on the panel.

## End-to-end example: "say X then nod twice then red light"

```sh
curl -s --noproxy '*' -X POST http://127.0.0.1:8766/say -H 'Content-Type: application/json' \
  -d '{"text":"测试成功"}'

# nod twice — issue back-to-back; the second waits for the first to finish
for i in 1 2; do
  curl -s --noproxy '*' -X POST http://127.0.0.1:8766/rpc -H 'Content-Type: application/json' \
    -d '{"method":"action","params":{"name":"nod"},"timeout":8}'
done

curl -s --noproxy '*' -X POST http://127.0.0.1:8766/rpc -H 'Content-Type: application/json' \
  -d '{"method":"set_led","params":{"r":255,"g":0,"b":0}}'
```

All 4 calls return `{"ok":true,...}`. If any returns `{"ok":false,"error":...}` STOP
and read the error — DON'T retry with a different method name, DON'T conclude
"firmware doesn't support it". The most common error you'll see is the agent
inventing names; double-check against the table above.

## When (and when not) to use 热狗

Use it when:
- User addresses 热狗 / "the desk robot" by name.
- The reply calls for **embodiment** — say a result aloud, nod yes, flash green on
  done. Adds presence beyond text.
- A small celebration / "I heard you" moment.

Don't use it when:
- The user is in a long focused task and 热狗 would just interrupt.
- The user explicitly asks for a text answer only.

Once per turn at most when it adds something — don't blink LEDs on every utterance.

## Composing moods (LED + motion + voice together)

| Mood | LED | Motion | Voice |
|---|---|---|---|
| happy | `(0, 80, 0)` warm green | `do_action nod` | short "好的!" |
| thinking | `(0, 0, 60)` blue | `do_action look_up` | (silent) |
| alert | `(80, 0, 0)` red | `do_action shake` | "注意" |
| reset / idle | `(0, 0, 0)` off | `do_action home` | (silent) |

## Hardware footnote

- Device: M5Stack CoreS3 (ESP32-S3) + StackChan base. On LAN at <device-ip>.
- Bridge: `bridge/server.py` on this Mac mini (<bridge-host>). Ports — 8765 WS to
  device, **8766 admin HTTP** (the one you call), 8767 MCP for Hermes auto-discovery.
- The MCP server at 127.0.0.1:8767 exposes the same operations as `hotdog_say` /
  `hotdog_do_action` / `hotdog_set_led` / `hotdog_get_sensors` / `hotdog_move_head`
  tools — equivalent to the HTTP recipes above, just typed as MCP tool calls.

## Compound flow: "take a photo of me"

1. `POST /find_face` — sweeps the head, locks on the user's face. Returns
   `{locked: true, x, y}` once aimed.
2. `POST /capture?format=json` — gets the JPEG bytes.
3. Reply to user with the photo (Telegram / Feishu can render the
   base64-decoded `jpeg_b64` as an inline image).

If `find_face` returns `locked: false`, tell the user "I don't see you" and
either nudge them ("come closer / face me") or retry once after a short delay.

## What's not yet wired (do NOT promise these to the user)

- Device-initiated voice (mic capture works on `/listen` POST, but the device can't
  yet *start* a conversation by tapping its head — there is no head-tap event yet)
- Real font rendering for `show_text` / `set_face` on the device — v1 paints flat
  colored bands. For actual text/face graphics, render server-side via Pillow and
  push with `show_image`.
- Sound effects / non-Doubao audio playback.

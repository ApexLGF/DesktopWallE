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
| `ping` | none | Liveness check |
| `move_head` | `{x, y, speed}` | Move head to absolute position. `x` -1280..1280 (yaw, -=left), `y` 30..870 (pitch, small=up, ~400=neutral), `speed` 0..1000. |
| `do_action` | `{name}` | Predefined gesture. `name` must be one of: `home`, `nod`, `shake`, `left`, `right`, `look_up`, `look_down`. |
| `set_led` | `{r, g, b}` or `{r, g, b, index}` | RGB ring. Without `index`: all 12 LEDs. With `index` 0..11: just that one. Each channel 0..255. |
| `set_face` | `{expr}` | Built-in face on the LCD. `expr` ∈ `neutral`, `happy`, `sad`, `surprised`, `sleepy`, `thinking`, `alert`. v1 paints flat colors as placeholders. |
| `show_text` | `{title, text}` | Show text on the LCD (v1 placeholder: paints a band; use `show_image` with rendered PNG for real glyphs). |
| `cam_capture` | `{sid?, quality?}` | Internal — don't call directly. The Bridge wraps this in `/capture`. |
| `get_sensors` | none | Returns `{battery_pct, heap_free, boot_count, wifi}`. |

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

### 2) Predefined gesture — `POST /rpc` with `do_action`

```sh
curl -s --noproxy '*' -X POST http://127.0.0.1:8766/rpc \
  -H 'Content-Type: application/json' \
  -d '{"method":"do_action","params":{"name":"nod"},"timeout":8}'
```

`name` must be one of `home | nod | shake | left | right | look_up | look_down`.
A nod or shake takes ~1.2 s; the call blocks until done.

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
```

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

### 10) Read sensors — `POST /rpc` with `get_sensors`

```sh
curl -s --noproxy '*' -X POST http://127.0.0.1:8766/rpc \
  -H 'Content-Type: application/json' \
  -d '{"method":"get_sensors"}'
```

Returns `{"ok":true,"result":{"battery_pct":100,"heap_free":...,"boot_count":...,"wifi":true}}`.

## End-to-end example: "say X then nod twice then red light"

```sh
curl -s --noproxy '*' -X POST http://127.0.0.1:8766/say -H 'Content-Type: application/json' \
  -d '{"text":"测试成功"}'

# nod twice — issue back-to-back; the second waits for the first to finish
for i in 1 2; do
  curl -s --noproxy '*' -X POST http://127.0.0.1:8766/rpc -H 'Content-Type: application/json' \
    -d '{"method":"do_action","params":{"name":"nod"},"timeout":8}'
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

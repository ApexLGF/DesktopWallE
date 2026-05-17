---
name: stackchan
description: "Control the M5 StackChan desk robot — make it speak (Doubao TTS), show an animated face, light its RGB LEDs, move its head, display text, and read its sensors. A physical presence for the Hermes Agent."
version: 1.0.0
author: Hermes Agent
license: MIT
platforms: [macos, linux]
metadata:
  hermes:
    tags: [Robot, Hardware, IoT, Voice, TTS, M5Stack, Desk-Toy, Smart-Home]
    related_skills: [openhue]
prerequisites:
  commands: [curl, python3]
---

# StackChan — the desk robot

StackChan is a small servo-driven robot head (M5Stack CoreS3 + StackChan kit) sitting
on the desk. It is a **peripheral for the Hermes Agent** — Hermes can give itself a
physical body: a voice, an expressive face, blinking LEDs, a head that nods and turns,
a screen, and a 9-axis motion sensor.

Everything is driven over plain HTTP from the Mac mini that runs Hermes. There is a
helper script that hides the curl/JSON/proxy details:

```
~/.hermes/skills/smart-home/stackchan/scripts/sc
```

Run `sc help` for the full command list. If the exec bit is ever lost, call it as
`bash ~/.hermes/skills/smart-home/stackchan/scripts/sc <args>`.

## When to Use

- User says "tell StackChan to …", "make the robot …", "have the desk robot say/show …"
- You want a physical reaction to something — speak a result aloud, nod yes, flash green
- You want StackChan to *be* the output device for an answer instead of (or alongside) chat
- Composing a "mood": face + LED + motion together so the robot feels alive
- Reading the robot's sensors (is it being touched? picked up? what's the battery?)

## When NOT to Use

- Philips Hue / general smart-home lights → use the `openhue` skill
- The robot is offline — `sc status` shows `devices: []`. Tell the user it's unplugged
  or off-network; do not retry blindly.
- Long-form output — the screen paginates but it is a tiny 320×240 LCD. Speak it or
  summarise; don't dump essays.
- **Taking a photo / "look at this" / camera** — the camera is currently **disabled**
  because it crashes the device (see Pitfalls). `sc look` will refuse. If the user asks
  StackChan to "see" something, explain the camera is out of order pending a firmware
  fix; do not try to work around it.

## Setup

Nothing to install — `curl` and `python3` ship with macOS. Two services must be up
(they normally already are, managed on the Mac mini):

- **stackproxy** — local daemon on `http://localhost:8766`. Does Doubao TTS and holds a
  persistent WebSocket to the device.
- **the device** — connects itself to stackproxy over WiFi. `sc status` confirms both.

The `sc` script auto-discovers the device's LAN IP from stackproxy, so it survives DHCP
changes. Override with `STACKCHAN_IP=<ip>` only if auto-discovery fails.

## Quick Reference

```bash
SC=~/.hermes/skills/smart-home/stackchan/scripts/sc

$SC status                          # device list + connection health — run this first
$SC say "你好，我是 StackChan"        # speak aloud — real Doubao TTS, no WAV needed
$SC face happy                      # animated expression
$SC move nod                        # named head choreography
$SC led rainbow                     # RGB LED effect
$SC led breathing 0 120 255 40      # effect + color (r g b) + speed_ms
$SC display "标题" "正文文字…"        # paginated text on the LCD
$SC big "✓ 完成"                     # one large centered string
$SC head 0.3 -0.2 600               # aim head: x,y normalized -1..1, speed
$SC sensors                         # IMU + compass + touch + battery
$SC events                          # drain physical events (taps, shakes, buttons)
$SC idle on                         # random subtle sway — leave ON when robot is idle
$SC breathing on                    # gentle pitch oscillation
```

### Speaking — this is the headline feature

**To make StackChan talk, just call `sc say "text"`.** stackproxy synthesises the
audio with Doubao TTS and streams it to the device. You do **not** need a WAV file, a
TTS service, an audio URL, or any conversion. If you ever catch yourself thinking *"I
have no TTS, I'll just show text on screen instead"* — that is wrong; `sc say` speaks.

Keep spoken text short and conversational (≤ ~80 Chinese chars). stackproxy strips
markdown/emoji/URLs and truncates long replies — write for the ear, not the page.

### Expressions (`sc face`)

`neutral happy smile love sad angry surprised thinking sleep wink_l wink_r stare dead
embarrassed cat talking` — the face auto-blinks.

### Head actions (`sc move`)

`home nod shake yes no left right look_up look_down look_around dance surprised sleep
wake panic peek tilt_left tilt_right bow`

### LED effects (`sc led`)

`off solid rainbow breathing pulse scanner wipe sparkle police fire chase theater
listening thinking talking recording`. `rainbow`/`fire` ignore the r/g/b args; the rest
use them. `recording` is the hard red blink the firmware shows while the mic is live.

## The two API surfaces

The `sc` script routes each command to the right place automatically. You only need to
know this when using the `sc dev` / `sc proxy` escape hatches:

| Surface | Base | What it serves |
|---|---|---|
| **stackproxy** | `http://localhost:8766/agent/*` | `say` (Doubao TTS), `express`, `move`, `status` (`look`/camera is disabled — see Pitfalls) |
| **device firmware** | `http://<device-ip>/...` | `led`, `display*`, `head*`, `sensors`, `events`, `idle`, `breathing`, `/action`, `/webhook` |

Escape hatches for anything the named commands don't cover:

```bash
$SC dev POST /display/marquee '{"text":"滚动字幕","step_ms":40}'
$SC dev GET /status
$SC proxy POST /agent/mic/toggle '{}'      # start/stop the device mic stream
```

The device firmware exposes ~34 endpoints (`/head/rotate`, `/camera/config`,
`/volume`, `/reboot`, `/display/progress`, `/webhook`, …). The shape is always
`POST` with `Content-Type: application/json`, response `{"ok":true|false,...}`.

## Composing a mood

Open-ended requests ("react to this", "be excited") are best answered with face + LED +
motion together:

| Mood | `sc face` | `sc led` | `sc move` |
|---|---|---|---|
| Excited | happy | rainbow | dance |
| Thinking | thinking | thinking | tilt_left |
| Sleepy | sleep | breathing | sleep |
| Alert | surprised | police | look_up |
| Loving | love | breathing 255 80 120 | nod |
| Annoyed | angry | fire | shake |

Fire the three commands back-to-back — the firmware runs LEDs / idle / face as
non-blocking effects. **Exception:** see the TTS pitfall below.

## Pitfalls

- **Don't send face/move RPC in the same instant as `sc say`.** While the device is
  playing TTS audio its WebSocket RPC handler is busy; `sc face` / `sc move` will return
  `{"ok":false,"error":"device timeout"}` for a few seconds. Either set the face/LED
  *before* `sc say`, or wait for the speech to finish. LED effects via the device
  surface are unaffected — only the stackproxy `express`/`move` RPCs collide.
- **Camera (`sc look`) is DISABLED — it wedges the device.** `/agent/look` triggers a
  camera capture that hits a firmware bug (esp_camera lib lockup). Confirmed in
  production on 2026-05-14: the device stops responding to WiFi, WebSocket *and* HTTP,
  and a WS reconnect does **not** recover it — it needs a **physical power cycle**.
  `sc look` refuses by default; the `--force` flag exists only for on-site debugging.
  If a user asks StackChan to "see" / "look at" / "take a photo", tell them the camera
  is out of order pending a firmware fix. Do not try `sc dev GET /camera/capture` as a
  workaround — that is the exact call that wedges it.
- **System proxy.** The Mac mini exports `http_proxy`. The `sc` script already passes
  `--noproxy '*'`; if you ever hand-write `curl`, you must do the same or requests to
  `localhost` / the device IP get hijacked.
- **Every POST needs `Content-Type: application/json`.** Without it the firmware
  replies `{"ok":false,"error":"missing json body"}`. `sc` sets it for you.
- **Offline device.** `sc status` → `devices: []` means the robot is unreachable. Report
  it; don't spin.

## Verification

```bash
SC=~/.hermes/skills/smart-home/stackchan/scripts/sc
$SC status                          # expect one device, last_seen_age_s small
$SC face happy && $SC led rainbow    # screen + LEDs change immediately
$SC say "技能加载成功"               # device speaks within ~1-2 s
$SC sensors                         # live IMU / battery JSON
```

If `status` shows the device and `say` produces audible speech, the skill is working
end to end.

# firmware-idf — ESP-IDF spike for esp-sr VAD on M5 CoreS3

This is a **proof-of-concept**, not the production firmware. It exists to
answer one question:

> Can we run `espressif/esp-sr` (specifically VADNet) on the M5Stack CoreS3
> + StackChan with ESP-IDF v5.5.4, and would the resulting VAD be good
> enough to drive bridge-side end-of-speech detection?

If the spike succeeds, the plan is to evolve this directory into the
production IDF firmware that eventually replaces `src/main.cpp` (Arduino).
The two firmwares are intentionally siblings — flashing one or the other
is a per-build choice, no shared sources.

## What this spike does (and doesn't do)

✅ Boot ESP-IDF v5.5.4 on the CoreS3
✅ Mount the `model` SPIFFS partition that esp-sr's CMake hooks populate
✅ Resolve and load `vadnet1_medium`
✅ Feed it a deterministic synthetic tone + silence stream
✅ Log VAD state transitions + heap accounting

❌ No I2S mic capture yet (Phase 2)
❌ No WiFi yet (Phase 3 — known to coexist OK with esp-sr per docs)
❌ No LCD / servos / LED / camera (handled by the Arduino firmware for now)
❌ No bridge protocol (the spike runs offline)

## Build, flash, monitor

```sh
. ~/esp/esp-idf/export.sh     # gets idf.py on PATH
cd firmware-idf
idf.py set-target esp32s3
idf.py menuconfig             # only if you want to tweak; defaults are fine
idf.py build
idf.py -p /dev/cu.usbmodem11301 flash monitor
```

Expected log on success:

```
I (xxx) vad_spike: ===== DesktopWallE IDF spike booting =====
I (xxx) vad_spike: IDF version: v5.5.4
I (xxx) vad_spike: [heap @ post-nvs] free internal=...  free psram=...
I (xxx) vad_spike: srmodel partition mounted: N entries
I (xxx) vad_spike:   [0] vadnet1_medium_cn
I (xxx) vad_spike: VADNet model selected: vadnet1_medium_cn
I (xxx) vad_spike: VADNet ready: sample_rate=16000 Hz  frame=480 samples (30 ms)
I (xxx) vad_spike: feeding 50 frames of speech, then 50 frames of silence
I (xxx) vad_spike:   frame    2  state SILENCE → SPEECH
I (xxx) vad_spike:   frame   83  state SPEECH → SILENCE
I (xxx) vad_spike: ===== spike result =====
I (xxx) vad_spike:   speech-detection lag : 2 frames (60 ms)
I (xxx) vad_spike:   silence-detection lag: 33 frames (990 ms after speech ended)
```

If `srmodel partition mounted: 0 entries` or the model name lookup fails,
the esp-sr CMake hooks didn't populate the partition — usually means
`CONFIG_SR_VADN_VADNET1_MEDIUM=y` is off, or `idf.py flash` was run with
a stale partition image. Re-run `idf.py build flash`.

## Status — 2026-05-17

Spike **green** on real CoreS3 hardware. Confirmed:

| Check | Result |
|---|---|
| IDF v5.5.4 + esp-sr 2.4.4 compile/link | ✅ |
| Bootloader → app → PSRAM init | ✅ (8 MB quad, NOT octal — common mistake) |
| `model` SPIFFS partition mounted | ✅ 1 entry: `vadnet1_medium` |
| VADNet handle creation | ✅ 32 ms frames @ 16 kHz |
| Memory budget | ✅ ~17 KB internal + ~310 KB PSRAM per handle |
| 300 Hz pure-tone classification | ✅ correctly NOT speech (as expected) |

Two gotchas worth recording for the next person:

1. **PSRAM is quad, not octal** on CoreS3 despite some sources saying octal.
   `CONFIG_SPIRAM_MODE_OCT=y` → bootloader aborts with "PSRAM chip is not
   connected, or wrong PSRAM line mode". Leave it on the default (quad).
2. **esp-sr from the component registry can't be downloaded reliably**
   from behind a Clash-style proxy — the archive (~300 MB with all
   wakenet model assets) truncates and you get "component.zip is not a
   zip file". We work around this by `EXTRA_COMPONENT_DIRS` to a local
   esp-sr checkout (see root `CMakeLists.txt`).

## Next phases (after this spike)

- **Phase 2**: hook up real I2S mic input (CoreS3 onboard NS4168 dual
  mic), replace the synthetic tone with live PCM, watch
  SILENCE→SPEECH→SILENCE transitions in serial.
- **Phase 3**: wire the bridge — send `mic.start` from bridge, stream
  raw PCM up via ws binary frames (the existing ocsc.v2 protocol), have
  device emit `mic.end` event when VADNet detects 1 s of trailing
  silence. Bridge's `_capture_utterance` already waits on `mic.end`, so
  no bridge changes needed.
- **Phase 4**: decide whether to fold the rest of the firmware
  (servos, LCD, LEDs, 34 HTTP endpoints) into this IDF project or keep
  the two-firmware setup.

## Why VADNet not lightweight VAD

esp-sr also exposes a non-neural VAD (`esp_vad.h` / `vad_create`). It's
~5 KB RAM and works with any 10/20/30 ms PCM frame. We picked VADNet for
the spike because:

1. The docs claim much better accuracy in noisy / mixed environments.
2. We're already paying the SPIFFS-partition cost for the production
   firmware (wakenet "你好热狗" + multinet would live there too).
3. The lightweight path is trivial to swap in if VADNet doesn't pan out
   (one-line change to `main.cpp` — `vad_create()` instead of
   `esp_vadn_handle_from_name()`).

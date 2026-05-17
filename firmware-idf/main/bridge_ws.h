// ocsc.v2 WebSocket client → bridge.
//
// Owns the persistent ws://STACKPROXY_WS_HOST:8765 connection. On
// (re)connect it sends a `hello` frame and waits for `hello.ack`. After
// that it routes:
//   - bridge → device: `req` RPCs (mic_start, tts_stop, …)
//   - device → bridge: `mic.start` / `mic.end` / generic `evt` frames
//                      + KIND_MIC_PCM binary frames
//
// VAD-driven `mic.end` is wired here so the main loop just calls
// bridge_ws_signal_speech_end() whenever VADNet flips SPEECH→SILENCE.

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// One-shot init. Spawns an internal task that connects and reconnects
// on its own; this function returns as soon as the first attempt has
// been scheduled. The actual ws is up asynchronously — check
// bridge_ws_is_connected() if you need to gate on it.
esp_err_t bridge_ws_start(const char *bridge_host, int bridge_port,
                           const char *device_id, uint32_t boot_count,
                           const char *fw_version);

// True after the ws is connected AND the hello handshake completed.
bool bridge_ws_is_connected(void);

// True after the bridge has sent a `mic_start` RPC and not yet sent
// `mic_stop`. Tells the main loop to start pumping PCM via the binary
// frame path.
bool bridge_ws_mic_streaming(void);

// Push a mic PCM frame to the bridge. n_samples is sample count (int16);
// payload bytes = n_samples * 2. Caller usually feeds 32 ms frames.
// Silently returns if ws disconnected / mic not streaming.
void bridge_ws_send_mic_pcm(const int16_t *samples, size_t n_samples);

// Tell the bridge "the user just stopped talking" — emits a `mic.end`
// evt frame, decrements internal sid, stops mic_streaming flag.
// Called by main loop on VADNet SPEECH→SILENCE transition while
// mic is open.
void bridge_ws_signal_speech_end(const char *reason);

// Mark that VAD observed at least one SPEECH frame during the current
// LISTEN turn. Called by the mic loop on every SILENCE→SPEECH edge.
// Cheap: just sets a volatile bool. Lets the `mic_stop` handler tell
// "the user spoke and stopped" apart from "bridge timed out without
// any speech" so the LCD doesn't lie about HEARD.
void bridge_ws_mark_speech_observed(void);

// True if SILENCE→SPEECH was observed during the current streaming
// turn. Used by the mic loop to gate `signal_speech_end` — without
// this, a SPEECH state that pre-dated mic_start (i.e., the user is
// still mid-wake-word when LISTEN opens) flips straight to HEARD on
// the wake-word's tail, before the actual command even starts.
bool bridge_ws_saw_speech(void);

// Emit `evt tts.done sid=<sid>` to the bridge. Called by speaker.cpp
// when the playback queue has drained past its end-of-utterance
// sentinel. Lets the bridge release its tts_lock and stop suppressing
// mic input.
void bridge_ws_send_tts_done(uint16_t sid);

// Emit `evt wake word=<word>` to the bridge. Called by the mic loop
// when esp-sr's WakeNet fires on the live mic stream. Bridge's
// voice_loop will pick this up and start a new conversation turn,
// equivalent to a manual /voice POST.
void bridge_ws_send_wake_event(const char *word);

#ifdef __cplusplus
}
#endif

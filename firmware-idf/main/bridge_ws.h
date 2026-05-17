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

#ifdef __cplusplus
}
#endif

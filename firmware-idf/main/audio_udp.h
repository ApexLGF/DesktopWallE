// audio_udp.h — UDP sidecar for high-volume audio (mic uplink, optionally
// TTS downlink). Bypasses esp-websocket-client's "any write timeout =
// fatal disconnect" behaviour that costs us a reconnect every few
// minutes during sustained mic streaming.
//
// Lifecycle:
//   audio_udp_start(host, port, device_id)
//     - allocates the socket, spawns recv + keepalive tasks
//     - DOES NOT yet send the probe; caller (bridge_ws) triggers
//       audio_udp_kick_probe() after the WS hello.ack arrives so we
//       only probe once we know bridge is alive.
//   audio_udp_kick_probe()
//     - sends KIND_UDP_HELLO with device_id payload to bridge:port
//     - bridge replies KIND_UDP_ACK → recv_task flips g_healthy=true
//   audio_udp_is_healthy()
//     - true once first KIND_UDP_ACK arrives. Cleared automatically
//       when no UDP_KA / UDP_ACK seen for >30 s (then we re-probe).
//   audio_udp_send_mic_pcm(pcm16, n_samples, sid, seq)
//     - sendto() once. No retry, no error propagation — losing one
//       32 ms frame is invisible to ASR. Returns false if not healthy
//       (caller should fall back to ws send_bin path).
//
// Design notes:
//   - Single ephemeral source port. NAT mapping is established by the
//     first UDP_HELLO; we keep it warm with periodic UDP_KA (15 s).
//   - recv_task uses select() with 1 s timeout so it can also drive the
//     health watchdog without a separate task.
//   - All transmits are non-blocking (MSG_DONTWAIT). EAGAIN → drop the
//     frame, never block the caller. This is the key property we lost
//     with esp-websocket-client.
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Allocate socket + spawn worker tasks. Called once at boot, before
// the first hello.ack. `bridge_host` is a hostname or IPv4 string —
// resolved synchronously here; if DNS fails the call returns ESP_FAIL
// and the device stays on the WS-only audio path.
esp_err_t audio_udp_start(const char *bridge_host,
                          int bridge_port,
                          const char *device_id);

// Send a KIND_UDP_HELLO with device_id as payload. Idempotent — the
// background watchdog also calls this whenever the link goes silent.
void audio_udp_kick_probe(void);

// True after the first KIND_UDP_ACK from bridge. Cleared when the
// path goes quiet (>30 s no rx).
bool audio_udp_is_healthy(void);

// Send one mic_pcm frame over UDP. Returns true on success, false if
// the UDP path is unhealthy or the sendto failed — caller should fall
// back to the WS path. Header layout matches the WS binary frames:
// version(1)+kind(1)+sid(LE16)+seq(LE32)+payload.
bool audio_udp_send_mic_pcm(const int16_t *pcm16,
                            size_t n_samples,
                            uint16_t sid,
                            uint32_t seq);

#ifdef __cplusplus
}
#endif

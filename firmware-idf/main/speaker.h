// CoreS3 speaker output via AW88298 (I2C addr 0x36).
//
// Shares the I2S0 TDM bus with the ES7210 mic — full-duplex. Audio is
// pushed via `speaker_play_pcm` from any task; an internal playback
// task drains a FreeRTOS queue and writes via esp_codec_dev. The
// caller marks end-of-utterance via `speaker_play_end(sid)` — when
// the queue drains to that sentinel, an `evt tts.done` is emitted up
// to the bridge so the server side can drop the tts_lock.
//
// Sample format: int16 mono LE @ 16 kHz (matches Doubao TTS output).

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t speaker_init(i2c_master_bus_handle_t i2c_bus, uint32_t sample_rate_hz);

// Start a new TTS playback session — clears any stale state. Safe to
// call repeatedly; consecutive calls just reset the sid.
void      speaker_play_start(uint16_t sid);

// Enqueue one chunk of mono int16 PCM. Copies the data internally, so
// the caller can free the buffer immediately. Drops the chunk and
// logs if the queue is full (back-pressure: bridge will see the gap
// in tts.done timing). `seq` is the per-utterance frame index from the
// ws binary header — used only for diagnostic timing logs.
void      speaker_play_pcm(const int16_t *pcm16, size_t n_samples, uint16_t seq);

// Enqueue one opus-encoded packet (60 ms frame, 16 kHz mono). Copies
// the data and queues into the decoder pipeline; an internal task
// decodes to PCM and forwards into the playback path. This is the
// preferred TTS path — opus packets are ~30 B (vs ~1920 B raw PCM),
// so WiFi delivery jitter has 30x less impact and back-pressure
// disappears even on weak links.
void      speaker_play_opus(const uint8_t *opus_pkt, size_t pkt_len, uint16_t seq);

// Signal end of the current utterance. Once the playback task drains
// the queue past this sentinel it emits `evt tts.done` and returns to
// idle. Safe to call even if the queue is currently empty.
void      speaker_play_end(uint16_t sid);

// Drop everything in flight (RPC `tts_stop` from bridge).
void      speaker_stop(void);

// True if PCM is currently queued or playing.
bool      speaker_is_active(void);

#ifdef __cplusplus
}
#endif

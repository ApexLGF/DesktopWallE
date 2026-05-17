// Minimal M5 CoreS3 mic adapter:
//
//   I2C bus (SDA=12, SCL=11) → ES7210 (mic ADC, default addr 0x40)
//   I2S RX (MCLK=0, BCLK=34, WS=33, DIN=14) → 16 kHz int16 mono PCM
//
// This is intentionally lean — no AEC, no AFE pipeline, just raw mic
// samples for feeding directly into the existing VADNet handle. The full
// AFE pipeline lands in Phase 3 once we know the model behaves on this
// hardware.

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"

#ifdef __cplusplus
extern "C" {
#endif

// Borrowed handle to the TX side of the shared TDM bus. speaker.cpp uses
// this to hook AW88298 onto the same I2S port — both chips share BCLK
// and WS, so they must share an i2s channel pair.
i2s_chan_handle_t mic_get_i2s_tx_handle(void);
i2s_port_t        mic_get_i2s_port(void);

// One-shot init. Pass the shared I2C bus handle so the codec sits on
// the same bus as the PMU (we don't want two masters on the same pins).
// Returns ESP_OK on success, an esp_err_t on failure (logged via
// ESP_LOGE — caller should treat as fatal for the spike).
esp_err_t mic_init(i2c_master_bus_handle_t i2c_bus,
                    uint32_t sample_rate_hz,
                    int      input_gain_db);

// Blocking read of `n_samples` int16_t samples (mono). Returns the
// number of samples actually read; <0 on error.
int mic_read(int16_t *out, size_t n_samples);

// Blocking read of `n_samples` PER CHANNEL across `mic_channel_count()`
// channels, interleaved (frame layout: ch0,ch1,ch2,ch0,ch1,ch2…).
// Buffer size = n_samples * channel_count * sizeof(int16_t).
int  mic_read_multi(int16_t *out, size_t n_samples);
int  mic_channel_count(void);

// Stop the codec / release I2S — only used at teardown.
void mic_deinit(void);

#ifdef __cplusplus
}
#endif

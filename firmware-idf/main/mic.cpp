// Mic adapter implementation — see mic.h.
//
// Built on esp_codec_dev's ES7210 driver. The ES7210 on CoreS3 is wired
// in TDM mode and naturally captures 4 slots (3 real mics + 1 reference
// loopback). We tell it `channel=1, channel_mask=bit0` so esp_codec_dev
// hands us only the first mic — VADNet only needs mono.
//
// Sample rate: VADNet is fixed at 16 kHz, so the codec runs at 16 kHz
// even though M5Stack's stock firmware uses 24 kHz. ES7210 supports
// 8/16/24/32/44.1/48 kHz natively; no resampling on our side.

#include "mic.h"

#include <string.h>

#include "esp_log.h"
#include "driver/i2c_master.h"
#include "driver/i2s_tdm.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"

static const char *TAG = "mic";

namespace {

constexpr i2c_port_num_t   I2C_PORT     = I2C_NUM_1;
constexpr gpio_num_t       I2C_SDA      = GPIO_NUM_12;
constexpr gpio_num_t       I2C_SCL      = GPIO_NUM_11;

constexpr i2s_port_t       I2S_PORT     = I2S_NUM_0;
constexpr gpio_num_t       I2S_MCLK     = GPIO_NUM_0;
constexpr gpio_num_t       I2S_BCLK     = GPIO_NUM_34;
constexpr gpio_num_t       I2S_WS       = GPIO_NUM_33;
constexpr gpio_num_t       I2S_DIN      = GPIO_NUM_14;

constexpr uint8_t          ES7210_ADDR  = ES7210_CODEC_DEFAULT_ADDR;

i2c_master_bus_handle_t      g_i2c_bus     = nullptr;  // borrowed, not owned
i2s_chan_handle_t            g_i2s_rx      = nullptr;
const audio_codec_data_if_t *g_data_if     = nullptr;
const audio_codec_ctrl_if_t *g_ctrl_if     = nullptr;
const audio_codec_if_t      *g_codec_if    = nullptr;
esp_codec_dev_handle_t       g_codec_dev   = nullptr;
int                          g_channels    = 1;

}  // namespace

esp_err_t mic_init(i2c_master_bus_handle_t i2c_bus,
                    uint32_t sample_rate_hz,
                    int      input_gain_db) {
    if (!i2c_bus) {
        ESP_LOGE(TAG, "mic_init: i2c_bus is null — call pmu_init() first to set up I2C");
        return ESP_ERR_INVALID_ARG;
    }
    g_i2c_bus = i2c_bus;

    // ─── I2S channel — RX only, TDM mode (ES7210 is TDM-only) ──────────
    {
        i2s_chan_config_t chan_cfg = {};
        chan_cfg.id                  = I2S_PORT;
        chan_cfg.role                = I2S_ROLE_MASTER;
        chan_cfg.dma_desc_num        = 6;
        chan_cfg.dma_frame_num       = 240;       // 15 ms @ 16 kHz
        chan_cfg.auto_clear_after_cb = true;
        esp_err_t err = i2s_new_channel(&chan_cfg, nullptr, &g_i2s_rx);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "i2s_new_channel failed: %s", esp_err_to_name(err));
            return err;
        }

        i2s_tdm_config_t tdm = {};
        tdm.clk_cfg.sample_rate_hz   = sample_rate_hz;
        tdm.clk_cfg.clk_src          = I2S_CLK_SRC_DEFAULT;
        tdm.clk_cfg.ext_clk_freq_hz  = 0;
        tdm.clk_cfg.mclk_multiple    = I2S_MCLK_MULTIPLE_256;
        tdm.clk_cfg.bclk_div         = 8;

        tdm.slot_cfg.data_bit_width  = I2S_DATA_BIT_WIDTH_16BIT;
        tdm.slot_cfg.slot_bit_width  = I2S_SLOT_BIT_WIDTH_AUTO;
        tdm.slot_cfg.slot_mode       = I2S_SLOT_MODE_STEREO;
        tdm.slot_cfg.slot_mask       = (i2s_tdm_slot_mask_t)(
            I2S_TDM_SLOT0 | I2S_TDM_SLOT1 | I2S_TDM_SLOT2 | I2S_TDM_SLOT3);
        tdm.slot_cfg.ws_width        = I2S_TDM_AUTO_WS_WIDTH;
        tdm.slot_cfg.ws_pol          = false;
        tdm.slot_cfg.bit_shift       = true;
        tdm.slot_cfg.left_align      = false;
        tdm.slot_cfg.big_endian      = false;
        tdm.slot_cfg.bit_order_lsb   = false;
        tdm.slot_cfg.skip_mask       = false;
        tdm.slot_cfg.total_slot      = I2S_TDM_AUTO_SLOT_NUM;

        tdm.gpio_cfg.mclk            = I2S_MCLK;
        tdm.gpio_cfg.bclk            = I2S_BCLK;
        tdm.gpio_cfg.ws              = I2S_WS;
        tdm.gpio_cfg.dout            = I2S_GPIO_UNUSED;
        tdm.gpio_cfg.din             = I2S_DIN;

        err = i2s_channel_init_tdm_mode(g_i2s_rx, &tdm);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "i2s init_tdm failed: %s", esp_err_to_name(err));
            return err;
        }
        err = i2s_channel_enable(g_i2s_rx);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "i2s_channel_enable failed: %s", esp_err_to_name(err));
            return err;
        }
    }

    // ─── esp_codec_dev: data + ctrl + ES7210 codec ─────────────────────
    {
        audio_codec_i2s_cfg_t i2s_cfg = {};
        i2s_cfg.port       = I2S_PORT;
        i2s_cfg.rx_handle  = g_i2s_rx;
        i2s_cfg.tx_handle  = nullptr;
        g_data_if = audio_codec_new_i2s_data(&i2s_cfg);
        if (!g_data_if) {
            ESP_LOGE(TAG, "audio_codec_new_i2s_data failed");
            return ESP_FAIL;
        }

        audio_codec_i2c_cfg_t i2c_cfg = {};
        i2c_cfg.port       = I2C_PORT;
        i2c_cfg.addr       = ES7210_ADDR;
        i2c_cfg.bus_handle = g_i2c_bus;
        g_ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
        if (!g_ctrl_if) {
            ESP_LOGE(TAG, "audio_codec_new_i2c_ctrl failed");
            return ESP_FAIL;
        }

        es7210_codec_cfg_t es7210_cfg = {};
        es7210_cfg.ctrl_if      = g_ctrl_if;
        // Match M5Stack stock firmware: enable all three ES7210 inputs.
        // On CoreS3 the physical mic is wired through one of these; the
        // others end up as silence or AEC reference. We read SLOT0 below
        // and rely on esp_codec_dev's channel_mask to multiplex.
        es7210_cfg.mic_selected = ES7210_SEL_MIC1 | ES7210_SEL_MIC2 | ES7210_SEL_MIC3;
        g_codec_if = es7210_codec_new(&es7210_cfg);
        if (!g_codec_if) {
            ESP_LOGE(TAG, "es7210_codec_new failed");
            return ESP_FAIL;
        }

        esp_codec_dev_cfg_t dev_cfg = {};
        dev_cfg.dev_type = ESP_CODEC_DEV_TYPE_IN;
        dev_cfg.codec_if = g_codec_if;
        dev_cfg.data_if  = g_data_if;
        g_codec_dev = esp_codec_dev_new(&dev_cfg);
        if (!g_codec_dev) {
            ESP_LOGE(TAG, "esp_codec_dev_new failed");
            return ESP_FAIL;
        }

        // Read all 4 TDM slots so the spike can compare them and pick
        // the right one for VAD. mic_read() (mono) will still hand back
        // slot 0; mic_read_multi() exposes all channels for inspection.
        g_channels = 4;
        esp_codec_dev_sample_info_t fs = {};
        fs.bits_per_sample = 16;
        fs.channel         = g_channels;
        fs.channel_mask    = (1 << g_channels) - 1;       // all enabled
        fs.sample_rate     = sample_rate_hz;
        esp_err_t err = esp_codec_dev_open(g_codec_dev, &fs);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_codec_dev_open failed: %s", esp_err_to_name(err));
            return err;
        }
        err = esp_codec_dev_set_in_gain(g_codec_dev, (float)input_gain_db);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "set_in_gain failed (non-fatal): %s", esp_err_to_name(err));
        }
    }

    ESP_LOGI(TAG, "mic ready: %u Hz, mono int16 (ES7210 SLOT0, gain=%d dB)",
             (unsigned)sample_rate_hz, input_gain_db);
    return ESP_OK;
}

// Internal scratch — multi-channel interleaved frame we then downmix
// to mono by picking ch0.
namespace { thread_local int16_t g_multi_scratch[2048]; }

int mic_read(int16_t *out, size_t n_samples) {
    if (!g_codec_dev || !out || n_samples == 0) return -1;
    if ((int)n_samples * g_channels > (int)(sizeof(g_multi_scratch) / sizeof(int16_t))) {
        ESP_LOGE(TAG, "mic_read: n_samples=%u too large", (unsigned)n_samples);
        return -1;
    }
    size_t bytes = n_samples * g_channels * sizeof(int16_t);
    esp_err_t err = esp_codec_dev_read(g_codec_dev, g_multi_scratch, bytes);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_codec_dev_read failed: %s", esp_err_to_name(err));
        return -1;
    }
    for (size_t i = 0; i < n_samples; ++i) out[i] = g_multi_scratch[i * g_channels];
    return (int)n_samples;
}

int mic_read_multi(int16_t *out, size_t n_samples) {
    if (!g_codec_dev || !out || n_samples == 0) return -1;
    size_t bytes = n_samples * g_channels * sizeof(int16_t);
    esp_err_t err = esp_codec_dev_read(g_codec_dev, out, bytes);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_codec_dev_read failed: %s", esp_err_to_name(err));
        return -1;
    }
    return (int)n_samples;
}

int mic_channel_count(void) { return g_channels; }

void mic_deinit(void) {
    if (g_codec_dev) {
        esp_codec_dev_close(g_codec_dev);
        esp_codec_dev_delete(g_codec_dev);
        g_codec_dev = nullptr;
    }
    if (g_codec_if) { audio_codec_delete_codec_if(g_codec_if); g_codec_if = nullptr; }
    if (g_ctrl_if)  { audio_codec_delete_ctrl_if(g_ctrl_if);   g_ctrl_if  = nullptr; }
    if (g_data_if)  { audio_codec_delete_data_if(g_data_if);   g_data_if  = nullptr; }
    if (g_i2s_rx) {
        i2s_channel_disable(g_i2s_rx);
        i2s_del_channel(g_i2s_rx);
        g_i2s_rx = nullptr;
    }
    // i2c_bus is borrowed from caller — don't free it here.
    g_i2c_bus = nullptr;
}

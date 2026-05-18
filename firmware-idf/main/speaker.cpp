// Speaker (AW88298) — see speaker.h.
//
// Pattern is lifted from M5Stack's official CoreS3 IDF firmware
// (cores3_audio_codec.cc, CoreS3AudioCodec class). The piece that bit
// us in the first cut: M5Stack `esp_codec_dev_new` in the constructor
// but defers `esp_codec_dev_open` until the moment audio is actually
// about to play (their EnableOutput(true)). Our first cut opened the
// codec at boot — which silently confused the AW88298 sample-rate
// auto-detect. Now we mirror their lazy-open: open on `tts.start`,
// close on `tts.end` after the queue drains.
//
// Hardware sharing:
//   - mic.cpp owns the I2S0 channel pair (RX TDM 4-slot, TX STD stereo).
//   - We borrow the TX handle via mic_get_i2s_tx_handle().
//   - AW88298 sits on the same I2C bus as the PMU / mic.

#include "speaker.h"

#include <string.h>
#include <stdlib.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_audio_dec.h"
#include "esp_opus_dec.h"

#include "mic.h"
#include "bridge_ws.h"
#include "lcd.h"

static const char *TAG = "spk";

namespace {

constexpr i2c_port_num_t   I2C_PORT     = I2C_NUM_1;
constexpr uint8_t          AW88298_ADDR = (0x36 << 1);
// esp_codec_dev_set_out_vol takes an INT in [1..100] (1=-49.5dB, 100=0dB).
constexpr int              DEFAULT_VOL_PCT = 80;
int                        g_volume_pct  = DEFAULT_VOL_PCT;
constexpr int              QUEUE_DEPTH    = 80;     // ~5 sec of 60-ms chunks
constexpr int              PREBUFFER_CHUNKS = 5;    // ~300 ms before first write
constexpr int              OPUS_QUEUE_DEPTH = 80;   // matches xiaozhi's 2.4 s decode buffer
constexpr int              OPUS_FRAME_DURATION_MS = 60;
constexpr int              OPUS_MAX_PACKET = 256;   // 60ms 16kHz mono opus rarely exceeds 100 B; 256 is safe

struct Chunk {
    int16_t  *pcm;        // mono int16, malloc'd; nullptr means control sentinel
    size_t    n_samples;  // valid when pcm != nullptr
    uint16_t  sid;
    uint16_t  seq;        // for diagnostic logs
    bool      start;      // tts.start — open codec
    bool      end;        // tts.end — close codec, fire tts.done
};

// Opus packets flow through a separate queue so the (slow) playback
// path doesn't back-pressure the (fast) network receive path. Decoder
// task drains this queue, decodes to PCM, then pushes into the chunk
// queue above where the speaker_task picks it up.
struct OpusItem {
    uint8_t  *pkt;
    size_t    pkt_len;
    uint16_t  sid;
    uint16_t  seq;
    bool      start;      // mirror of tts.start — reset decoder + open codec
    bool      end;        // mirror of tts.end — drain decoder + close codec
};

i2c_master_bus_handle_t      g_i2c_bus    = nullptr;
const audio_codec_data_if_t *g_data_if    = nullptr;
const audio_codec_ctrl_if_t *g_ctrl_if    = nullptr;
const audio_codec_gpio_if_t *g_gpio_if    = nullptr;
const audio_codec_if_t      *g_codec_if   = nullptr;
esp_codec_dev_handle_t       g_codec_dev  = nullptr;
QueueHandle_t                g_queue      = nullptr;     // PCM chunks ready to play
QueueHandle_t                g_opus_queue = nullptr;     // opus packets pending decode
TaskHandle_t                 g_task       = nullptr;
TaskHandle_t                 g_dec_task   = nullptr;
void                        *g_opus_dec   = nullptr;     // esp_opus_dec handle
volatile bool                g_active     = false;
volatile uint16_t            g_cur_sid    = 0;
uint32_t                     g_sample_rate = 16000;
bool                         g_is_open    = false;

void open_for_playback() {
    if (g_is_open) return;
    esp_codec_dev_sample_info_t fs = {};
    fs.bits_per_sample = 16;
    fs.channel         = 1;
    fs.channel_mask    = 0;
    fs.sample_rate     = g_sample_rate;
    esp_err_t err = esp_codec_dev_open(g_codec_dev, &fs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "codec_dev_open: %s", esp_err_to_name(err));
        return;
    }
    esp_codec_dev_set_out_vol(g_codec_dev, g_volume_pct);
    esp_codec_dev_set_out_mute(g_codec_dev, false);
    g_is_open = true;
}

void close_after_playback() {
    if (!g_is_open) return;
    esp_codec_dev_close(g_codec_dev);
    g_is_open = false;
}

void open_opus_decoder() {
    if (g_opus_dec) return;
    esp_opus_dec_cfg_t cfg = {};
    cfg.sample_rate    = g_sample_rate;
    cfg.channel        = 1;
    cfg.frame_duration = ESP_OPUS_DEC_FRAME_DURATION_60_MS;
    cfg.self_delimited = false;
    esp_audio_err_t ret = esp_opus_dec_open(&cfg, sizeof(cfg), &g_opus_dec);
    if (ret != ESP_AUDIO_ERR_OK || !g_opus_dec) {
        ESP_LOGE(TAG, "esp_opus_dec_open failed: %d", (int)ret);
        g_opus_dec = nullptr;
    } else {
        ESP_LOGI(TAG, "opus decoder ready (16k mono 60ms)");
    }
}

void close_opus_decoder() {
    if (!g_opus_dec) return;
    esp_opus_dec_close(g_opus_dec);
    g_opus_dec = nullptr;
}

// Decoder task: drain the opus queue, decode each packet to PCM, then
// push decoded PCM into the play queue. By default an opus frame at
// 16 kHz mono 60 ms decodes to 960 int16 samples = 1920 bytes. We
// allocate a fresh PCM buffer per chunk; speaker_task will free it.
void decode_task(void *arg) {
    (void)arg;
    OpusItem item{};
    while (true) {
        if (xQueueReceive(g_opus_queue, &item, portMAX_DELAY) != pdTRUE) continue;
        if (item.start) {
            open_opus_decoder();
            // Mirror the start sentinel into the play queue so the
            // speaker_task opens its codec.
            Chunk c = { nullptr, 0, item.sid, 0, true, false };
            xQueueSend(g_queue, &c, pdMS_TO_TICKS(50));
            continue;
        }
        if (item.end) {
            // No decoder flush needed for opus (per-packet model).
            Chunk c = { nullptr, 0, item.sid, 0, false, true };
            xQueueSend(g_queue, &c, pdMS_TO_TICKS(50));
            close_opus_decoder();
            continue;
        }
        if (!item.pkt) continue;
        if (!g_opus_dec) {
            free(item.pkt);
            ESP_LOGW(TAG, "decode: no decoder open, dropping packet");
            continue;
        }

        // Each 60 ms @ 16 kHz mono → 960 samples = 1920 bytes.
        constexpr size_t MAX_PCM = OPUS_FRAME_DURATION_MS * 16 * 2;  // = 1920
        int16_t *pcm = (int16_t *)malloc(MAX_PCM);
        if (!pcm) {
            free(item.pkt);
            ESP_LOGW(TAG, "decode: OOM");
            continue;
        }
        esp_audio_dec_in_raw_t raw = {};
        raw.buffer = item.pkt;
        raw.len    = (uint32_t)item.pkt_len;
        esp_audio_dec_out_frame_t out = {};
        out.buffer = (uint8_t *)pcm;
        out.len    = MAX_PCM;
        esp_audio_dec_info_t info = {};
        esp_audio_err_t ret = esp_opus_dec_decode(g_opus_dec, &raw, &out, &info);
        free(item.pkt);
        if (ret != ESP_AUDIO_ERR_OK) {
            ESP_LOGW(TAG, "opus_decode seq=%u failed: %d", (unsigned)item.seq, (int)ret);
            free(pcm);
            continue;
        }
        size_t n_samples = out.decoded_size / sizeof(int16_t);
        Chunk c = { pcm, n_samples, item.sid, item.seq, false, false };
        if (xQueueSend(g_queue, &c, pdMS_TO_TICKS(200)) != pdTRUE) {
            ESP_LOGW(TAG, "play queue full — drop decoded seq=%u", (unsigned)item.seq);
            free(pcm);
        }
    }
}

void speaker_task(void *arg) {
    (void)arg;
    Chunk c{};
    bool prebuffering = false;  // between tts.start and the moment we begin playing
    while (true) {
        if (xQueueReceive(g_queue, &c, portMAX_DELAY) != pdTRUE) continue;
        if (c.start) {
            open_for_playback();
            prebuffering = true;
            continue;
        }
        if (c.end) {
            prebuffering = false;
            g_active = false;
            // The last esp_codec_dev_write returned the moment the DMA
            // descriptor accepted our bytes, but the I2S DMA still has
            // up to ~240 ms of audio queued (dma_desc_num=8 *
            // dma_frame_num=480 / 16 kHz, see mic.cpp). If we send
            // tts.done immediately and close the codec, the bridge
            // opens the follow-up mic over the bot's own tail audio
            // and SenseVoice ASRs it back as a "user turn" — a
            // feedback loop where the device chats with itself.
            //
            // Hold here while the DMA drains naturally (codec stays
            // open so playout continues), then report tts.done, then
            // close. 300 ms covers the 240 ms DMA cushion plus a
            // small physical cone-decay / room-reverb margin.
            vTaskDelay(pdMS_TO_TICKS(300));
            bridge_ws_send_tts_done(c.sid);
            close_after_playback();
            // Settle the LCD back to IDLE if the bridge has nothing
            // queued. We learned the hard way that 2 s is too aggressive
            // — Hermes-driven bridges can take 3-4 s before sending the
            // next `mic_start` (agent post-processing, hooks, etc.). The
            // user then sees a TALK → IDLE → LISTEN flicker right as
            // they're starting to speak the next turn. 5 s gives breathing
            // room; any state change cancels the timer.
            lcd_arm_idle_in(5000);
            continue;
        }
        if (!c.pcm) continue;
        if (!g_is_open) open_for_playback();

        // Small jitter buffer: gather PREBUFFER_CHUNKS (or up to 400 ms)
        // of decoded PCM before the first write so the DMA queue starts
        // out comfortably full. With OPUS the network arrival is fast
        // enough that this almost always returns in < 200 ms.
        if (prebuffering) {
            UBaseType_t qlen = uxQueueMessagesWaiting(g_queue);
            if (qlen < (UBaseType_t)PREBUFFER_CHUNKS) {
                int64_t wait_start = esp_timer_get_time();
                while (qlen < (UBaseType_t)PREBUFFER_CHUNKS &&
                       (esp_timer_get_time() - wait_start) < 400000) {
                    vTaskDelay(pdMS_TO_TICKS(20));
                    qlen = uxQueueMessagesWaiting(g_queue);
                }
            }
            prebuffering = false;
        }

        esp_err_t err = esp_codec_dev_write(g_codec_dev, c.pcm,
                                              c.n_samples * sizeof(int16_t));
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "codec_dev_write %s — dropping chunk", esp_err_to_name(err));
        }
        free(c.pcm);
    }
}

}  // namespace

esp_err_t speaker_init(i2c_master_bus_handle_t i2c_bus, uint32_t sample_rate_hz) {
    if (!i2c_bus) return ESP_ERR_INVALID_ARG;
    g_i2c_bus     = i2c_bus;
    g_sample_rate = sample_rate_hz;

    i2s_chan_handle_t i2s_tx = mic_get_i2s_tx_handle();
    if (!i2s_tx) {
        ESP_LOGE(TAG, "mic_init() must run first (i2s tx not allocated)");
        return ESP_ERR_INVALID_STATE;
    }

    audio_codec_i2s_cfg_t i2s_cfg = {};
    i2s_cfg.port      = mic_get_i2s_port();
    i2s_cfg.rx_handle = nullptr;
    i2s_cfg.tx_handle = i2s_tx;
    g_data_if = audio_codec_new_i2s_data(&i2s_cfg);
    if (!g_data_if) { ESP_LOGE(TAG, "new_i2s_data failed"); return ESP_FAIL; }

    audio_codec_i2c_cfg_t i2c_cfg = {};
    i2c_cfg.port       = I2C_PORT;
    i2c_cfg.addr       = AW88298_ADDR;
    i2c_cfg.bus_handle = g_i2c_bus;
    g_ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    if (!g_ctrl_if) { ESP_LOGE(TAG, "new_i2c_ctrl(aw88298) failed"); return ESP_FAIL; }

    g_gpio_if = audio_codec_new_gpio();

    aw88298_codec_cfg_t aw_cfg = {};
    aw_cfg.ctrl_if                    = g_ctrl_if;
    aw_cfg.gpio_if                    = g_gpio_if;
    aw_cfg.reset_pin                  = -1;
    aw_cfg.hw_gain.pa_voltage         = 5.0f;
    aw_cfg.hw_gain.codec_dac_voltage  = 3.3f;
    aw_cfg.hw_gain.pa_gain            = 1;
    g_codec_if = aw88298_codec_new(&aw_cfg);
    if (!g_codec_if) { ESP_LOGE(TAG, "aw88298_codec_new failed"); return ESP_FAIL; }

    esp_codec_dev_cfg_t dev_cfg = {};
    dev_cfg.dev_type = ESP_CODEC_DEV_TYPE_OUT;
    dev_cfg.codec_if = g_codec_if;
    dev_cfg.data_if  = g_data_if;
    g_codec_dev = esp_codec_dev_new(&dev_cfg);
    if (!g_codec_dev) { ESP_LOGE(TAG, "esp_codec_dev_new failed"); return ESP_FAIL; }

    // Note: NO esp_codec_dev_open here. M5Stack's reference defers
    // open() until just before the first Write — opens get paired with
    // a real audio stream so AW88298's Fs detector sees valid clocks.

    g_queue = xQueueCreate(QUEUE_DEPTH, sizeof(Chunk));
    if (!g_queue) return ESP_ERR_NO_MEM;
    g_opus_queue = xQueueCreate(OPUS_QUEUE_DEPTH, sizeof(OpusItem));
    if (!g_opus_queue) return ESP_ERR_NO_MEM;

    BaseType_t ok = xTaskCreatePinnedToCore(speaker_task, "spk", 6144, nullptr,
                                              configMAX_PRIORITIES - 4, &g_task, 1);
    if (ok != pdPASS) return ESP_ERR_NO_MEM;
    ok = xTaskCreatePinnedToCore(decode_task, "spk_dec", 8192, nullptr,
                                   configMAX_PRIORITIES - 5, &g_dec_task, 0);
    if (ok != pdPASS) return ESP_ERR_NO_MEM;

    ESP_LOGI(TAG, "speaker ready (AW88298 on I2C 0x%02x, lazy-open)",
             AW88298_ADDR >> 1);
    return ESP_OK;
}

void speaker_play_start(uint16_t sid) {
    if (!g_opus_queue) return;
    g_cur_sid = sid;
    g_active = true;
    // Send start sentinel through the opus pipeline. The decode_task
    // forwards a Chunk{start=true} into the play queue so the speaker
    // codec opens before any decoded PCM arrives.
    OpusItem item = { nullptr, 0, sid, 0, true, false };
    xQueueSend(g_opus_queue, &item, pdMS_TO_TICKS(50));
    ESP_LOGI(TAG, "tts.start sid=%u (opus path)", (unsigned)sid);
}

void speaker_play_pcm(const int16_t *pcm16, size_t n_samples, uint16_t seq) {
    if (!g_queue || !pcm16 || n_samples == 0) return;
    int16_t *copy = (int16_t *)malloc(n_samples * sizeof(int16_t));
    if (!copy) {
        ESP_LOGW(TAG, "OOM on chunk (%u samples)", (unsigned)n_samples);
        return;
    }
    memcpy(copy, pcm16, n_samples * sizeof(int16_t));
    Chunk c = { copy, n_samples, g_cur_sid, seq, false, false };
    if (xQueueSend(g_queue, &c, pdMS_TO_TICKS(50)) != pdTRUE) {
        ESP_LOGW(TAG, "queue full — drop %u samples", (unsigned)n_samples);
        free(copy);
    }
}

void speaker_play_end(uint16_t sid) {
    if (!g_opus_queue) return;
    OpusItem item = { nullptr, 0, sid, 0, false, true };
    xQueueSend(g_opus_queue, &item, pdMS_TO_TICKS(50));
}

void speaker_play_opus(const uint8_t *opus_pkt, size_t pkt_len, uint16_t seq) {
    if (!g_opus_queue || !opus_pkt || pkt_len == 0) return;
    if (pkt_len > OPUS_MAX_PACKET) {
        ESP_LOGW(TAG, "opus pkt too large: %u — dropping", (unsigned)pkt_len);
        return;
    }
    uint8_t *copy = (uint8_t *)malloc(pkt_len);
    if (!copy) { ESP_LOGW(TAG, "OOM on opus copy"); return; }
    memcpy(copy, opus_pkt, pkt_len);
    OpusItem item = { copy, pkt_len, g_cur_sid, seq, false, false };
    if (xQueueSend(g_opus_queue, &item, pdMS_TO_TICKS(50)) != pdTRUE) {
        ESP_LOGW(TAG, "opus queue full — drop seq=%u", (unsigned)seq);
        free(copy);
    }
}

void speaker_stop(void) {
    if (g_opus_queue) {
        OpusItem item{};
        while (xQueueReceive(g_opus_queue, &item, 0) == pdTRUE) {
            if (item.pkt) free(item.pkt);
        }
    }
    if (g_queue) {
        Chunk c{};
        while (xQueueReceive(g_queue, &c, 0) == pdTRUE) {
            if (c.pcm) free(c.pcm);
        }
    }
    g_active = false;
}

bool speaker_is_active(void) { return g_active; }

esp_err_t speaker_set_volume(int pct) {
    if (pct < 0 || pct > 100) return ESP_ERR_INVALID_ARG;
    g_volume_pct = pct;
    if (g_codec_dev && g_is_open) {
        esp_codec_dev_set_out_vol(g_codec_dev, pct);
    }
    ESP_LOGI(TAG, "volume = %d%%", pct);
    return ESP_OK;
}

int speaker_get_volume(void) {
    return g_codec_dev ? g_volume_pct : -1;
}

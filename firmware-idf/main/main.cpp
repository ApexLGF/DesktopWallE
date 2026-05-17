// DesktopWallE — ESP-IDF spike, Phase 3: device ↔ bridge wired up.
//
// Boot order:
//   nvs → srmodel/vadnet → I2C → pmu (AXP2101 + AW9523) → mic (ES7210)
//   → wifi STA → ocsc.v2 ws to bridge → hello → wait
//
// Steady state (mic loop):
//   - read 32 ms PCM (ch2 = real mic on CoreS3) → VADNet
//   - if bridge has called mic_start, every frame is pushed up as a
//     KIND_MIC_PCM binary frame
//   - on SPEECH→SILENCE transition (1 s trailing-silence latency baked
//     into VADNet's min_noise_ms), emit `mic.end` text frame. Bridge's
//     _capture_utterance picks this up and stops waiting on RMS.

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_mac.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/i2c_master.h"
#include "driver/gpio.h"

#include "esp_vad.h"
#include "esp_vadn_iface.h"
#include "esp_vadn_models.h"
#include "model_path.h"

#include "mic.h"
#include "pmu.h"
#include "wifi_sta.h"
#include "bridge_ws.h"
#include "lcd.h"
#include "speaker.h"
#include "led.h"

// Re-use the Arduino firmware's config so SSID / bridge host live in
// one place. (gitignored)
#if __has_include("../../include/config.h")
#  include "../../include/config.h"
#else
#  warning "include/config.h not found — using empty defaults. Copy include/config.example.h."
#  define STACKCHAN_WIFI_SSID     ""
#  define STACKCHAN_WIFI_PASSWORD ""
#  define STACKPROXY_WS_HOST      ""
#  define STACKPROXY_WS_PORT      8765
#endif

static const char *TAG = "vad_spike";

namespace {

constexpr uint32_t kMicSampleRateHz = 16000;
constexpr int      kMicGainDb       = 75;      // 2-ch open spreads gain; max preamp brings level back up
constexpr int      kVadChannel      = 1;       // ES7210 2-ch: peak diagnostics show slot 1 carries the real mic
constexpr char     kFwVersion[]     = "idf-0.1.0";

void heap_dump(const char *where) {
    ESP_LOGI(TAG, "[heap @ %s] free internal=%u  free psram=%u  largest internal=%u",
             where,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
}

// Pull a stable device_id from MAC + bump a boot_count counter in NVS
// so the bridge can de-duplicate stale ghost sessions.
void load_device_id_and_boot(char *device_id_out, size_t cap, uint32_t *boot_count_out) {
    uint8_t mac[6] = {0};
    esp_efuse_mac_get_default(mac);
    snprintf(device_id_out, cap, "hotdog-%02x%02x%02x%02x%02x%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    nvs_handle_t h = 0;
    if (nvs_open("dw", NVS_READWRITE, &h) == ESP_OK) {
        uint32_t bc = 0;
        nvs_get_u32(h, "boot", &bc);
        bc += 1;
        nvs_set_u32(h, "boot", bc);
        nvs_commit(h);
        nvs_close(h);
        *boot_count_out = bc;
    } else {
        *boot_count_out = 0;
    }
}

}  // namespace

extern "C" void app_main(void) {
    ESP_LOGI(TAG, "===== DesktopWallE IDF spike (Phase 3: ws bridge) =====");
    ESP_LOGI(TAG, "IDF version: %s", esp_get_idf_version());

    {
        esp_err_t err = nvs_flash_init();
        if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
            ESP_ERROR_CHECK(nvs_flash_erase());
            err = nvs_flash_init();
        }
        ESP_ERROR_CHECK(err);
    }
    heap_dump("post-nvs");

    char     device_id[64];
    uint32_t boot_count = 0;
    load_device_id_and_boot(device_id, sizeof(device_id), &boot_count);
    ESP_LOGI(TAG, "device: %s  boot=%lu", device_id, (unsigned long)boot_count);

    // ─── VADNet ─────────────────────────────────────────────────────────
    srmodel_list_t *models = esp_srmodel_init("model");
    if (!models) { ESP_LOGE(TAG, "srmodel_init failed"); return; }
    char *vadn_name = esp_srmodel_filter(models, ESP_VADN_PREFIX, NULL);
    auto *vadnet = (esp_vadn_iface_t *)esp_vadn_handle_from_name(vadn_name);
    model_iface_data_t *model = vadnet->create(vadn_name, VAD_MODE_1, 1, 128, 1000);
    const int frame_samps = vadnet->get_samp_chunksize(model);
    const int model_sr    = vadnet->get_samp_rate(model);
    ESP_LOGI(TAG, "VADNet: %d Hz × %d samples (%d ms)", model_sr, frame_samps,
             frame_samps * 1000 / model_sr);

    // ─── I2C bus + PMU + Mic ────────────────────────────────────────────
    i2c_master_bus_handle_t i2c_bus = nullptr;
    {
        i2c_master_bus_config_t cfg = {};
        cfg.clk_source                   = I2C_CLK_SRC_DEFAULT;
        cfg.i2c_port                     = I2C_NUM_1;
        cfg.scl_io_num                   = GPIO_NUM_11;
        cfg.sda_io_num                   = GPIO_NUM_12;
        cfg.glitch_ignore_cnt            = 7;
        cfg.flags.enable_internal_pullup = true;
        ESP_ERROR_CHECK(i2c_new_master_bus(&cfg, &i2c_bus));
    }
    pmu_init(i2c_bus);
    if (led_init(i2c_bus) != ESP_OK) {
        ESP_LOGW(TAG, "led init failed — RGB ring will stay dark");
    }
    vTaskDelay(pdMS_TO_TICKS(100));
    if (mic_init(i2c_bus, kMicSampleRateHz, kMicGainDb) != ESP_OK) {
        ESP_LOGE(TAG, "mic_init failed");
        return;
    }
    if (speaker_init(i2c_bus, kMicSampleRateHz) != ESP_OK) {
        ESP_LOGW(TAG, "speaker_init failed — TTS will be silent");
    }
    // ─── LCD ───────────────────────────────────────────────────────────
    // Bring up the screen before WiFi so the user has visual feedback
    // ("BOOT" → "WIFI") instead of staring at a black panel for ~3 s.
    if (lcd_init() != ESP_OK) {
        ESP_LOGW(TAG, "lcd init failed — continuing headless");
    }
    heap_dump("post-audio+lcd");

    // ─── WiFi STA ───────────────────────────────────────────────────────
    lcd_set_state(LCD_STATE_WIFI);
    if (wifi_sta_connect(STACKCHAN_WIFI_SSID, STACKCHAN_WIFI_PASSWORD) != ESP_OK) {
        ESP_LOGE(TAG, "wifi connect failed — continuing offline (VAD-only)");
        lcd_set_state(LCD_STATE_ERROR);
    }

    // ─── ocsc.v2 ws to bridge ───────────────────────────────────────────
    lcd_set_state(LCD_STATE_BRIDGE);
    if (STACKPROXY_WS_HOST[0]) {
        bridge_ws_start(STACKPROXY_WS_HOST, STACKPROXY_WS_PORT,
                        device_id, boot_count, kFwVersion);
    } else {
        ESP_LOGW(TAG, "STACKPROXY_WS_HOST empty — running offline");
        lcd_set_state(LCD_STATE_ERROR);
    }
    heap_dump("post-ws");

    // ─── Mic streaming + VAD loop ───────────────────────────────────────
    const int nch = mic_channel_count();
    int16_t *multi = (int16_t *)heap_caps_malloc(frame_samps * nch * sizeof(int16_t),
                                                   MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    int16_t *mono  = (int16_t *)heap_caps_malloc(frame_samps * sizeof(int16_t),
                                                   MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!multi || !mono) { ESP_LOGE(TAG, "OOM mic frame buffer"); return; }

    ESP_LOGI(TAG, "===== mic loop running — talk to the device =====");

    vad_state_t prev          = VAD_SILENCE;
    int64_t     t0_us         = esp_timer_get_time();
    int         frames        = 0;
    int         heartbeat_period = 1000 / (frame_samps * 1000 / model_sr);   // ≈31 frames
    bool        prev_streaming = false;
    int         idle_paint_at  = -1;     // when (in frames) to flip LCD back to IDLE

    while (true) {
        int got = mic_read_multi(multi, frame_samps);
        if (got <= 0) {
            ESP_LOGE(TAG, "mic_read_multi returned %d", got);
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        for (int i = 0; i < frame_samps; ++i) mono[i] = multi[i * nch + kVadChannel];

        // Ship PCM up to bridge whenever it's asked for it (mic_streaming
        // flag flips on at mic_start RPC, off at mic.end / mic_stop).
        if (bridge_ws_mic_streaming()) {
            bridge_ws_send_mic_pcm(mono, frame_samps);
        }

        vad_state_t cur = vadnet->detect(model, mono);
        ++frames;

        if (cur != prev) {
            int64_t ms = (esp_timer_get_time() - t0_us) / 1000;
            ESP_LOGI(TAG, ">>> t=%6lld ms  %-7s → %-7s  ws=%d streaming=%d",
                     ms,
                     prev == VAD_SPEECH ? "SPEECH" : "SILENCE",
                     cur  == VAD_SPEECH ? "SPEECH" : "SILENCE",
                     bridge_ws_is_connected(), bridge_ws_mic_streaming());
            // SPEECH→SILENCE while streaming = end-of-utterance.
            if (prev == VAD_SPEECH && cur == VAD_SILENCE && bridge_ws_mic_streaming()) {
                bridge_ws_signal_speech_end("vad");
                lcd_set_state(LCD_STATE_HEARD);
            }
            prev = cur;
        }
        // Schedule LCD → IDLE shortly after the mic stream closes so the
        // user gets a clear "we're done, talk again to wake" cue.
        bool streaming_now = bridge_ws_mic_streaming();
        if (prev_streaming && !streaming_now) {
            idle_paint_at = frames + heartbeat_period * 3;   // ~3 s later
        }
        prev_streaming = streaming_now;
        if (idle_paint_at > 0 && frames >= idle_paint_at) {
            if (bridge_ws_is_connected()) lcd_set_state(LCD_STATE_IDLE);
            idle_paint_at = -1;
        }

        if ((frames % heartbeat_period) == 0) {
            ESP_LOGI(TAG, "[hb] t=%6lld ms frames=%d  ws=%d streaming=%d  state=%s",
                     (esp_timer_get_time() - t0_us) / 1000,
                     frames,
                     bridge_ws_is_connected(),
                     bridge_ws_mic_streaming(),
                     cur == VAD_SPEECH ? "SPEECH" : "SILENCE");
        }
    }
}

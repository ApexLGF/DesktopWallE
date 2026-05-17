// DesktopWallE — ESP-IDF spike for esp-sr VAD on M5 CoreS3.
//
// Goal: prove ESP-IDF v5.5.4 + esp-sr VADNet can boot and load on the
// target hardware. No mic capture, no UI, no WiFi yet — just:
//   1. Boot, mount NVS.
//   2. esp_srmodel_init("model") finds the SPIFFS model partition.
//   3. Resolve the vadnet entry and create a handle.
//   4. Feed it a deterministic synthetic chunk and print state transitions.
//   5. Heap accounting before/after to confirm reasonable memory use.
//
// Once green, the next milestone is wiring it to a real I2S mic feed.

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_vad.h"
#include "esp_vadn_iface.h"
#include "esp_vadn_models.h"
#include "model_path.h"

static const char *TAG = "vad_spike";

// 16 kHz mono int16 sine burst (~300 Hz @ peak 8000) → should register as
// VAD_SPEECH. Followed by a chunk of silence → should flip back to
// VAD_SILENCE within `min_noise_ms`. We synthesize on the fly to avoid
// shipping a wav file in the spike.
static void fill_tone(int16_t *buf, size_t samples, int freq_hz,
                      int sr_hz, int16_t peak) {
    for (size_t i = 0; i < samples; ++i) {
        float t = (float)i / (float)sr_hz;
        buf[i] = (int16_t)(peak * sinf(2.0f * (float)M_PI * (float)freq_hz * t));
    }
}

static void fill_silence(int16_t *buf, size_t samples) {
    memset(buf, 0, samples * sizeof(int16_t));
}

static void heap_dump(const char *where) {
    ESP_LOGI(TAG, "[heap @ %s] free internal=%u  free psram=%u  largest internal=%u",
             where,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
}

extern "C" void app_main(void) {
    ESP_LOGI(TAG, "===== DesktopWallE IDF spike booting =====");
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

    // ─── esp-sr model partition ─────────────────────────────────────────
    srmodel_list_t *models = esp_srmodel_init("model");
    if (!models) {
        ESP_LOGE(TAG, "esp_srmodel_init(\"model\") returned NULL — is the "
                       "`model` partition populated? Re-flash with `idf.py flash`.");
        return;
    }
    ESP_LOGI(TAG, "srmodel partition mounted: %d entries", models->num);
    for (int i = 0; i < models->num; ++i) {
        ESP_LOGI(TAG, "  [%d] %s", i, models->model_name[i]);
    }

    char *vadn_name = esp_srmodel_filter(models, ESP_VADN_PREFIX, NULL);
    if (!vadn_name) {
        ESP_LOGE(TAG, "no VADNet model present. Check sdkconfig: "
                       "CONFIG_USE_VADNET=y + CONFIG_SR_VADN_CN_VADNET1_MEDIUM=y");
        esp_srmodel_deinit(models);
        return;
    }
    ESP_LOGI(TAG, "VADNet model selected: %s", vadn_name);

    esp_vadn_iface_t *vadnet =
        (esp_vadn_iface_t *)esp_vadn_handle_from_name(vadn_name);
    if (!vadnet) {
        ESP_LOGE(TAG, "esp_vadn_handle_from_name failed");
        esp_srmodel_deinit(models);
        return;
    }

    // VADNet trigger parameters — match the docs' "common" config:
    //   min_speech_ms = 128  (drop bursts shorter than this)
    //   min_noise_ms  = 1000 (1 s of silence to flip back)
    //   mode          = VAD_MODE_1 (Aggressive)
    model_iface_data_t *model = vadnet->create(vadn_name, VAD_MODE_1, 1, 128, 1000);
    if (!model) {
        ESP_LOGE(TAG, "vadnet->create failed");
        esp_srmodel_deinit(models);
        return;
    }
    heap_dump("post-vadnet-create");

    int sr_hz       = vadnet->get_samp_rate(model);
    int frame_samps = vadnet->get_samp_chunksize(model);
    ESP_LOGI(TAG, "VADNet ready: sample_rate=%d Hz  frame=%d samples (%d ms)",
             sr_hz, frame_samps, frame_samps * 1000 / sr_hz);

    // ─── Synthetic test stream: 1.5 s speech (tone) + 1.5 s silence ─────
    int16_t *buf = (int16_t *)heap_caps_malloc(frame_samps * sizeof(int16_t),
                                                 MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!buf) {
        ESP_LOGE(TAG, "OOM allocating frame buffer");
        vadnet->destroy(model);
        esp_srmodel_deinit(models);
        return;
    }

    const int total_speech_frames  = (sr_hz * 1500 / 1000) / frame_samps;
    const int total_silence_frames = (sr_hz * 1500 / 1000) / frame_samps;
    vad_state_t prev = VAD_SILENCE;
    int speech_first = -1, silence_first = -1;

    ESP_LOGI(TAG, "feeding %d frames of speech, then %d frames of silence",
             total_speech_frames, total_silence_frames);

    for (int i = 0; i < total_speech_frames; ++i) {
        fill_tone(buf, frame_samps, 300, sr_hz, 8000);
        vad_state_t s = vadnet->detect(model, buf);
        if (s != prev) {
            ESP_LOGI(TAG, "  frame %4d  state %s → %s", i,
                     prev == VAD_SPEECH ? "SPEECH" : "SILENCE",
                     s    == VAD_SPEECH ? "SPEECH" : "SILENCE");
            if (s == VAD_SPEECH && speech_first < 0) speech_first = i;
            prev = s;
        }
    }
    for (int i = 0; i < total_silence_frames; ++i) {
        fill_silence(buf, frame_samps);
        vad_state_t s = vadnet->detect(model, buf);
        if (s != prev) {
            ESP_LOGI(TAG, "  frame %4d  state %s → %s",
                     i + total_speech_frames,
                     prev == VAD_SPEECH ? "SPEECH" : "SILENCE",
                     s    == VAD_SPEECH ? "SPEECH" : "SILENCE");
            if (s == VAD_SILENCE && silence_first < 0) silence_first = i;
            prev = s;
        }
    }

    ESP_LOGI(TAG, "===== spike result =====");
    ESP_LOGI(TAG, "  speech-detection lag : %d frames (%d ms)",
             speech_first, speech_first < 0 ? -1 : speech_first * frame_samps * 1000 / sr_hz);
    ESP_LOGI(TAG, "  silence-detection lag: %d frames (%d ms after speech ended)",
             silence_first, silence_first < 0 ? -1 : silence_first * frame_samps * 1000 / sr_hz);

    free(buf);
    vadnet->destroy(model);
    esp_srmodel_deinit(models);
    heap_dump("post-cleanup");

    ESP_LOGI(TAG, "spike done; idling forever");
    while (true) vTaskDelay(pdMS_TO_TICKS(60000));
}

// DesktopWallE — ESP-IDF spike, Phase 2: live mic → VADNet on M5 CoreS3.
//
// Boots, loads vadnet1_medium, opens ES7210 mic over I2S @ 16 kHz mono,
// then continuously feeds 32 ms frames into VADNet and logs every
// SPEECH ⇄ SILENCE transition with a timestamp. This is the proof that
// device-side VAD can replace bridge-side RMS guessing.
//
// Talk into the mic and watch the serial — you should see SILENCE→SPEECH
// land within ~30–60 ms of voice onset, and SPEECH→SILENCE after the
// configured min_noise_ms (1 s by default) of trailing quiet.

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_vad.h"
#include "esp_vadn_iface.h"
#include "esp_vadn_models.h"
#include "model_path.h"

#include "mic.h"

static const char *TAG = "vad_spike";

namespace {

constexpr uint32_t kMicSampleRateHz = 16000;
constexpr int      MIC_GAIN_DB    = 60;   // matches M5Stack stock firmware

void heap_dump(const char *where) {
    ESP_LOGI(TAG, "[heap @ %s] free internal=%u  free psram=%u  largest internal=%u",
             where,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
}

}  // namespace

extern "C" void app_main(void) {
    ESP_LOGI(TAG, "===== DesktopWallE IDF spike (Phase 2: live mic) =====");
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

    // ─── esp-sr model + VADNet ──────────────────────────────────────────
    srmodel_list_t *models = esp_srmodel_init("model");
    if (!models) {
        ESP_LOGE(TAG, "esp_srmodel_init(\"model\") returned NULL");
        return;
    }
    char *vadn_name = esp_srmodel_filter(models, ESP_VADN_PREFIX, NULL);
    if (!vadn_name) {
        ESP_LOGE(TAG, "no VADNet model in partition");
        esp_srmodel_deinit(models);
        return;
    }
    ESP_LOGI(TAG, "VADNet model: %s", vadn_name);

    auto *vadnet = (esp_vadn_iface_t *)esp_vadn_handle_from_name(vadn_name);
    if (!vadnet) {
        ESP_LOGE(TAG, "esp_vadn_handle_from_name failed");
        esp_srmodel_deinit(models);
        return;
    }

    // min_speech_ms=128, min_noise_ms=1000 — the bridge wants ~1 s of
    // trailing silence to decide "user stopped talking". VAD_MODE_1 is
    // a balanced sensitivity; if we get false-positives in noisy rooms
    // bump to MODE_2 or MODE_3.
    model_iface_data_t *model = vadnet->create(vadn_name, VAD_MODE_1, 1, 128, 1000);
    if (!model) {
        ESP_LOGE(TAG, "vadnet->create failed");
        esp_srmodel_deinit(models);
        return;
    }
    const int frame_samps = vadnet->get_samp_chunksize(model);
    const int model_sr    = vadnet->get_samp_rate(model);
    ESP_LOGI(TAG, "VADNet ready: %d Hz × %d samples (%d ms/frame)",
             model_sr, frame_samps, frame_samps * 1000 / model_sr);
    heap_dump("post-vadnet-create");

    if ((uint32_t)model_sr != kMicSampleRateHz) {
        ESP_LOGE(TAG, "VADNet sample rate %d ≠ mic rate %u — this spike "
                       "doesn't resample, abort.", model_sr, (unsigned)kMicSampleRateHz);
        vadnet->destroy(model);
        esp_srmodel_deinit(models);
        return;
    }

    // ─── Mic ────────────────────────────────────────────────────────────
    if (mic_init(kMicSampleRateHz, MIC_GAIN_DB) != ESP_OK) {
        ESP_LOGE(TAG, "mic_init failed");
        vadnet->destroy(model);
        esp_srmodel_deinit(models);
        return;
    }
    heap_dump("post-mic-init");

    const int nch = mic_channel_count();
    int16_t *multi = (int16_t *)heap_caps_malloc(frame_samps * nch * sizeof(int16_t),
                                                   MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    int16_t *mono  = (int16_t *)heap_caps_malloc(frame_samps * sizeof(int16_t),
                                                   MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!multi || !mono) {
        ESP_LOGE(TAG, "OOM mic frame buffer");
        return;
    }

    ESP_LOGI(TAG, "===== streaming mic → VADNet — %d TDM channels — talk to the device =====", nch);

    auto rms_of_ch = [&](const int16_t *p, int n_frames, int nch_, int ch) -> int {
        long long s = 0;
        for (int i = 0; i < n_frames; ++i) {
            int v = p[i * nch_ + ch];
            s += (long long)v * v;
        }
        return (int)(s / (n_frames > 0 ? n_frames : 1));
    };
    auto absmax_of_ch = [&](const int16_t *p, int n_frames, int nch_, int ch) -> int {
        int m = 0;
        for (int i = 0; i < n_frames; ++i) {
            int v = p[i * nch_ + ch];
            int a = v < 0 ? -v : v;
            if (a > m) m = a;
        }
        return m;
    };

    vad_state_t prev    = VAD_SILENCE;
    int64_t     t0_us   = esp_timer_get_time();
    int         frames  = 0;
    int         speech_frames = 0;
    int         heartbeat_period = 1000 / (frame_samps * 1000 / model_sr);   // ≈31 frames

    while (true) {
        int got = mic_read_multi(multi, frame_samps);
        if (got <= 0) {
            ESP_LOGE(TAG, "mic_read_multi returned %d, retrying", got);
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        // De-interleave ch0 → mono buffer for VADNet feed
        for (int i = 0; i < frame_samps; ++i) mono[i] = multi[i * nch];

        vad_state_t cur = vadnet->detect(model, mono);
        ++frames;
        if (cur == VAD_SPEECH) ++speech_frames;

        if (cur != prev) {
            int64_t ms_since_boot = (esp_timer_get_time() - t0_us) / 1000;
            ESP_LOGI(TAG, ">>> t=%6lld ms  frame=%5d  %-7s → %-7s",
                     ms_since_boot, frames,
                     prev == VAD_SPEECH ? "SPEECH" : "SILENCE",
                     cur  == VAD_SPEECH ? "SPEECH" : "SILENCE");
            prev = cur;
        }

        // 1 Hz heartbeat — log RMS+peak for every channel so we can see
        // which TDM slot has the actual mic input.
        if ((frames % heartbeat_period) == 0) {
            char chinfo[200] = {0};
            int  off         = 0;
            for (int c = 0; c < nch; ++c) {
                int r = rms_of_ch(multi, frame_samps, nch, c);
                int p = absmax_of_ch(multi, frame_samps, nch, c);
                off += snprintf(chinfo + off, sizeof(chinfo) - off,
                                "  ch%d[rms=%d peak=%d]", c, r, p);
            }
            ESP_LOGI(TAG, "[hb] t=%6lld ms  frames=%d  state=%s  speech_pct=%.1f%% %s",
                     (esp_timer_get_time() - t0_us) / 1000,
                     frames,
                     cur == VAD_SPEECH ? "SPEECH" : "SILENCE",
                     100.0 * speech_frames / frames,
                     chinfo);
        }
    }
}

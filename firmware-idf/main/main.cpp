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
#include "esp_wn_iface.h"
#include "esp_wn_models.h"
#include "model_path.h"

#include "mic.h"
#include "pmu.h"
#include "wifi_sta.h"
#include "bridge_ws.h"
#include "audio_udp.h"
#include "lcd.h"
#include "speaker.h"
#include "led.h"
#include "touch.h"
#include "servo.h"

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

// Forehead-tap handler. The "interrupt the robot" gesture:
//   - mid-TTS: cut the speaker immediately so the user can talk;
//   - any state: emit `evt wake word=tap` so the bridge cancels any
//     in-flight TTS generation and opens a new turn (same path
//     WakeNet uses, so the bridge only learns one shape).
// pad_index is 0..2 or -1 (multi-pad). We don't differentiate yet —
// any tap means "stop and listen."
void on_touch_tap(int pad_index) {
    ESP_LOGI(TAG, "💆 head tap pad=%d — interrupting", pad_index);
    if (speaker_is_active()) {
        speaker_stop();
    }
    bridge_ws_send_wake_event("tap");
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

    // ─── VADNet + WakeNet ───────────────────────────────────────────────
    srmodel_list_t *models = esp_srmodel_init("model");
    if (!models) { ESP_LOGE(TAG, "srmodel_init failed"); return; }
    char *vadn_name = esp_srmodel_filter(models, ESP_VADN_PREFIX, NULL);
    auto *vadnet = (esp_vadn_iface_t *)esp_vadn_handle_from_name(vadn_name);
    // Signature: create(name, mode, channels, min_speech_ms, min_noise_ms).
    // min_noise_ms = trailing silence required to flip SPEECH→SILENCE. 1000 ms
    // was too eager — users naturally pause >1 s mid-sentence ("嗯…", thinking
    // beats), and we were cutting them off and emitting HEARD prematurely.
    // 2000 ms gives breathing room without making the device feel sluggish to
    // wrap up at a real end-of-utterance.
    model_iface_data_t *model = vadnet->create(vadn_name, VAD_MODE_1, 1, 128, 2000);
    const int frame_samps = vadnet->get_samp_chunksize(model);
    const int model_sr    = vadnet->get_samp_rate(model);
    ESP_LOGI(TAG, "VADNet: %d Hz × %d samples (%d ms)", model_sr, frame_samps,
             frame_samps * 1000 / model_sr);

    char *wn_name = esp_srmodel_filter(models, ESP_WN_PREFIX, NULL);
    esp_wn_iface_t      *wakenet  = nullptr;
    model_iface_data_t  *wn_model = nullptr;
    int                  wn_chunk = 0;
    if (wn_name) {
        wakenet  = (esp_wn_iface_t *)esp_wn_handle_from_name(wn_name);
        wn_model = wakenet->create(wn_name, DET_MODE_90);
        wn_chunk = wakenet->get_samp_chunksize(wn_model);
        // Threshold tuning history:
        //   0.63 — model default, tuned for clean AFE-processed audio.
        //          Never fires on our raw-mic pipeline.
        //   0.40 — Compensates for our 75 dB raw gain + no AEC.
        //          Earlier "false wakes from typing" turned out to be
        //          mis-classified Si12T touch events leaking through as
        //          taps, not WakeNet itself. With MIN_TAP_LEVEL=2 on the
        //          touch driver, this threshold was safe to leave low.
        //   0.50 — Bumped after user reported the desktop robot
        //          self-waking with no one saying "Hi Wall-E". Still too
        //          eager.
        //   0.55 — current. Next notch. If still too eager, step to 0.60.
        //          If the wake word itself stops registering, drop back
        //          to 0.50 + consider enabling AFE pre-processing.
        wakenet->set_det_threshold(wn_model, 0.55f, 1);
        float thr = wakenet->get_det_threshold(wn_model, 1);
        ESP_LOGI(TAG, "WakeNet: model=%s chunk=%d threshold=%.2f — \"Hi Wall-E\"",
                 wn_name, wn_chunk, thr);
    } else {
        ESP_LOGW(TAG, "no WakeNet model in flash — hands-free wake disabled");
    }

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
    touch_register_tap_cb(on_touch_tap);
    if (touch_init(i2c_bus) != ESP_OK) {
        ESP_LOGW(TAG, "touch init failed — head-tap interrupt disabled");
    }
    // Servo power rail is gated on PY32 GPIO0; without this HIGH the
    // SCS servos are dark on the bus and uart_write has no effect.
    if (led_set_servo_power(true) != ESP_OK) {
        ESP_LOGW(TAG, "could not enable servo power rail");
    }
    if (servo_init() == ESP_OK) {
        // Give the bus a beat to settle after power-up before sending.
        vTaskDelay(pdMS_TO_TICKS(200));
        servo_home();
    } else {
        ESP_LOGW(TAG, "servo init failed — head actions disabled");
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
        // Audio UDP sidecar — same host as bridge, fixed port 8768.
        // The actual probe (KIND_UDP_HELLO) is kicked from bridge_ws
        // when the ws hello.ack arrives — so the order here is: open
        // socket + spawn worker tasks now; send first packet later
        // once we know bridge is listening.
        if (audio_udp_start(STACKPROXY_WS_HOST, 8768, device_id) != ESP_OK) {
            ESP_LOGW(TAG, "audio_udp_start failed — mic stays on WS");
        }
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
    // Frame-count threshold for "this is real speech, not just wake-word
    // tail". 1280 ms ≈ 40 frames at 32 ms/frame. Wake word "Hi Wall-E"
    // streamed-during-mic tail is typically ~700 ms (mic_start arrives
    // partway through the wake word). 40 frames is safely above wake-tail
    // duration but reachable by any real command.
    const int   speech_real_frames = (40 * 32) / (frame_samps * 1000 / model_sr);
    int         speech_frames_listen = 0;     // cumulative SPEECH while streaming
    bool        prev_streaming_listen = false;

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

        // WakeNet runs only when we're truly idle: NOT mid-recording AND
        // NOT mid-playback. Without the speaker_is_active guard, every
        // TTS reply re-triggers wake on the mic capturing the speaker's
        // own audio bleed — which both wastes CPU (audible stuttering)
        // and makes the LCD flip between TALK/LISTEN. xiaozhi handles
        // this with proper AEC; we skip the issue by simply pausing.
        if (wakenet && wn_model && wn_chunk == frame_samps &&
                !bridge_ws_mic_streaming() && !speaker_is_active()) {
            int idx = wakenet->detect(wn_model, mono);
            if (idx > 0) {
                ESP_LOGI(TAG, "🎤 WAKE detected (idx=%d)", idx);
                bridge_ws_send_wake_event("hi_walle");
            }
        }

        if (cur != prev) {
            int64_t ms = (esp_timer_get_time() - t0_us) / 1000;
            ESP_LOGI(TAG, ">>> t=%6lld ms  %-7s → %-7s  ws=%d streaming=%d",
                     ms,
                     prev == VAD_SPEECH ? "SPEECH" : "SILENCE",
                     cur  == VAD_SPEECH ? "SPEECH" : "SILENCE",
                     bridge_ws_is_connected(), bridge_ws_mic_streaming());
            // SILENCE→SPEECH edge during streaming: clear "real speech" tag.
            if (prev == VAD_SILENCE && cur == VAD_SPEECH) {
                bridge_ws_mark_speech_observed();
            }
            // SPEECH→SILENCE while streaming = candidate end-of-utterance.
            // Two layers of confidence required to actually fire mic.end:
            //   1) bridge_ws_saw_speech()  — either a fresh SILENCE→SPEECH
            //      edge happened during streaming, OR cumulative SPEECH
            //      passed the speech_real_frames threshold (handles the
            //      "user speaks immediately after wake with no SILENCE gap"
            //      case where no fresh edge ever fires).
            //   2) without that gate, wake-word's tail SPEECH→SILENCE trips
            //      mic.end before the user has even begun their command.
            if (prev == VAD_SPEECH && cur == VAD_SILENCE &&
                    bridge_ws_mic_streaming() && bridge_ws_saw_speech()) {
                bridge_ws_signal_speech_end("vad");
                lcd_set_state(LCD_STATE_HEARD);
            }
            prev = cur;
        }
        // Cumulative-SPEECH path: also mark saw_speech once enough
        // streaming SPEECH frames accumulate, even without a fresh
        // SILENCE→SPEECH edge. Required for the continuous-speech case.
        {
            bool now_streaming = bridge_ws_mic_streaming();
            if (!prev_streaming_listen && now_streaming) {
                speech_frames_listen = 0;     // fresh mic_start, reset counter
            }
            prev_streaming_listen = now_streaming;
            if (now_streaming && cur == VAD_SPEECH) {
                ++speech_frames_listen;
                if (speech_frames_listen == speech_real_frames) {
                    bridge_ws_mark_speech_observed();
                    ESP_LOGI(TAG, "vad cumulative SPEECH passed %d frames → saw_speech",
                             speech_real_frames);
                }
            }
        }
        // LCD → IDLE used to fire here on mic-stream end, but that races
        // the bridge — Hermes can take 80 s to think and bridge wouldn't
        // send "思考中" until partway through. The IDLE flicker between
        // HEARD and THINK was misleading. Now lcd_arm_idle_in() is called
        // from speaker.cpp once TTS playback actually drains, which is
        // the only "we're truly done" signal the device has.

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

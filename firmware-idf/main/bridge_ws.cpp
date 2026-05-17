// ocsc.v2 client — see bridge_ws.h.
//
// Uses espressif/esp_websocket_client. The client lives in its own
// internal task; we get events via the registered handler and route
// incoming text frames through cJSON.

#include "bridge_ws.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_websocket_client.h"
#include "esp_event.h"
#include "esp_timer.h"
#include "cJSON.h"

#include "lcd.h"
#include "speaker.h"
#include "led.h"

static const char *TAG = "bridge_ws";

namespace {

// Match bridge/protocol.py exactly.
constexpr uint8_t PROTOCOL_VERSION = 0x02;
constexpr uint8_t KIND_MIC_PCM     = 0x01;
constexpr uint8_t KIND_TTS_PCM     = 0x02;
constexpr uint8_t KIND_TTS_OPUS    = 0x06;

constexpr size_t BINARY_HEADER_LEN = 8;
// 8 bytes: u8 ver, u8 kind, u16 sid LE, u32 seq LE

esp_websocket_client_handle_t g_client = nullptr;
bool g_connected      = false;
bool g_hello_acked    = false;
bool g_mic_streaming  = false;
uint16_t g_mic_sid    = 0;
uint32_t g_mic_seq    = 0;
volatile bool g_saw_speech = false;   // any SPEECH frame observed this turn?
uint16_t g_tts_sid    = 0;
uint32_t g_tts_seq    = 0;
bool     g_tts_in_flight = false;

char g_device_id[64]    = {0};
uint32_t g_boot_count   = 0;
char g_fw_version[16]   = {0};

void send_hello() {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "t", "hello");
    cJSON_AddStringToObject(o, "device_id", g_device_id);
    cJSON_AddNumberToObject(o, "boot_count", g_boot_count);
    // connection_id: just a one-shot hex string based on boot time so
    // bridge ghost-session logic has something to key on.
    char cid[24];
    snprintf(cid, sizeof(cid), "%08lx", (unsigned long)(esp_timer_get_time() & 0xFFFFFFFF));
    cJSON_AddStringToObject(o, "connection_id", cid);
    cJSON_AddStringToObject(o, "fw", g_fw_version);
    cJSON *caps = cJSON_CreateArray();
    cJSON_AddItemToArray(caps, cJSON_CreateString("listen"));
    cJSON_AddItemToArray(caps, cJSON_CreateString("vad"));
    cJSON_AddItemToArray(caps, cJSON_CreateString("wake"));
    cJSON_AddItemToObject(o, "caps", caps);
    char *json = cJSON_PrintUnformatted(o);
    esp_websocket_client_send_text(g_client, json, strlen(json), portMAX_DELAY);
    free(json);
    cJSON_Delete(o);
    ESP_LOGI(TAG, "→ hello %s boot=%lu", g_device_id, (unsigned long)g_boot_count);
}

void send_text(const char *json) {
    if (!g_client || !g_connected) return;
    esp_websocket_client_send_text(g_client, json, strlen(json), pdMS_TO_TICKS(1000));
}

void send_res_ok(const char *rpc_id, cJSON *data /*may be nullptr*/) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "t", "res");
    cJSON_AddStringToObject(o, "id", rpc_id);
    cJSON_AddBoolToObject(o, "ok", true);
    if (data) cJSON_AddItemToObject(o, "d", data);
    char *json = cJSON_PrintUnformatted(o);
    send_text(json);
    free(json);
    cJSON_Delete(o);
}

void send_res_err(const char *rpc_id, const char *code, const char *msg) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "t", "err");
    cJSON_AddStringToObject(o, "id", rpc_id);
    cJSON_AddStringToObject(o, "code", code);
    cJSON_AddStringToObject(o, "msg", msg ? msg : "");
    char *json = cJSON_PrintUnformatted(o);
    send_text(json);
    free(json);
    cJSON_Delete(o);
}

void handle_req(cJSON *root) {
    const cJSON *id_j = cJSON_GetObjectItemCaseSensitive(root, "id");
    const cJSON *m_j  = cJSON_GetObjectItemCaseSensitive(root, "m");
    if (!cJSON_IsString(m_j) || !cJSON_IsString(id_j)) {
        ESP_LOGW(TAG, "malformed req: missing id/m");
        return;
    }
    const char *rpc_id = id_j->valuestring;
    const char *method = m_j->valuestring;
    ESP_LOGI(TAG, "← req id=%s m=%s", rpc_id, method);
    if (strcmp(method, "mic_start") == 0) {
        const cJSON *p_j   = cJSON_GetObjectItemCaseSensitive(root, "p");
        const cJSON *sid_j = p_j ? cJSON_GetObjectItemCaseSensitive(p_j, "sid") : nullptr;
        uint16_t sid = cJSON_IsNumber(sid_j) ? (uint16_t)sid_j->valueint : 0;
        if (!sid) sid = (uint16_t)(esp_timer_get_time() & 0xFFFF) | 0x8000;
        g_mic_sid       = sid;
        g_mic_seq       = 0;
        g_mic_streaming = true;
        g_saw_speech    = false;
        send_res_ok(rpc_id, nullptr);
        char buf[96];
        snprintf(buf, sizeof(buf),
                 "{\"t\":\"mic.start\",\"sid\":%u,\"sr\":16000,\"fmt\":\"pcm16\"}",
                 (unsigned)sid);
        send_text(buf);
        // Visible cue on the LCD so the user knows to speak.
        lcd_set_state(LCD_STATE_LISTENING);
        ESP_LOGI(TAG, "mic streaming on, sid=%u (LCD=LISTEN)", (unsigned)sid);
    } else if (strcmp(method, "mic_stop") == 0) {
        bool was_streaming = g_mic_streaming;
        bool had_speech    = g_saw_speech;
        g_mic_streaming = false;
        send_res_ok(rpc_id, nullptr);
        // mic_stop → "ASR" state (we sent audio, bridge is now running
        // its server-side Doubao Flash ASR). Subsequent show_text frames
        // from bridge ("已听到 ✓" → HEARD, "请再说一次" → ASR_ERR, "没听到"
        // → IDLE, "思考中" → THINK) advance the state. The 15 s auto-IDLE
        // fallback handles the case where bridge has nothing more to say.
        lcd_set_state(LCD_STATE_ASR);
        lcd_arm_idle_in(15000);
        if (was_streaming && !had_speech) {
            // Hint to bridge that local VAD never saw speech — bridge can
            // skip the Doubao call entirely (saves ~1.5 s + ¥).
            send_text("{\"t\":\"evt\",\"name\":\"mic.timeout\",\"d\":{\"reason\":\"no_speech\"}}");
            ESP_LOGI(TAG, "mic streaming off (no speech → LCD=ASR, IDLE in 15s)");
        } else {
            ESP_LOGI(TAG, "mic streaming off (LCD=ASR, IDLE in 15s)");
        }
    } else if (strcmp(method, "show_text") == 0) {
        // Bridge sends rich text we don't fully render yet — peek at the
        // title to infer agent state and update the LCD color accordingly.
        const cJSON *p_j     = cJSON_GetObjectItemCaseSensitive(root, "p");
        const cJSON *title_j = p_j ? cJSON_GetObjectItemCaseSensitive(p_j, "title") : nullptr;
        if (cJSON_IsString(title_j)) {
            const char *t = title_j->valuestring;
            // titles we recognize (in priority order):
            //   "思考中"     → THINKING   "已听到" → HEARD
            //   "回复"       → SPEAKING   "请讲"   → LISTENING
            //   "继续吗"     → LISTENING  "识别中" → ASR (server doing Doubao Flash)
            //   "请再说"     → ASR_ERR (retry prompt — ASR returned nothing)
            //   "没听清"     → ASR_ERR
            //   "识别失败"   → ASR_ERR
            //   "没听到"     → IDLE (no audio captured / RMS-only noise — bridge skipped ASR)
            //   anything else → leave state untouched
            // Most-specific patterns must be checked before broader ones
            // (e.g. "识别失败" before plain "识别"), and any 请-prefixed
            // title that's NOT "请讲" should land on ASR_ERR.
            if (strstr(t, "\xe6\x80\x9d\xe8\x80\x83")) {
                lcd_set_state(LCD_STATE_THINKING);
                // Title is like "思考中… 5秒" — pull the digits out so we
                // can render the counter on the panel under "THINK".
                int sec = -1;
                for (const char *p = t; *p; ++p) {
                    if (*p >= '0' && *p <= '9') {
                        sec = 0;
                        while (*p >= '0' && *p <= '9') {
                            sec = sec * 10 + (*p - '0');
                            ++p;
                        }
                        break;
                    }
                }
                if (sec >= 0) lcd_set_think_elapsed(sec);
            }
            else if (strstr(t, "\xe5\xb7\xb2\xe5\x90\xac")) lcd_set_state(LCD_STATE_HEARD);
            else if (strstr(t, "\xe5\x9b\x9e\xe5\xa4\x8d")) lcd_set_state(LCD_STATE_SPEAKING);
            // 识别失败 = E8 AF 86 E5 88 AB E5 A4 B1 E8 B4 A5
            // ASR errors are terminal for the conversation turn — bridge
            // already ended its voice_loop. Arm a 3 s auto-IDLE so the
            // user sees the error briefly then the screen returns to a
            // ready state, rather than sitting on ASR ERR forever.
            else if (strstr(t, "\xe8\xaf\x86\xe5\x88\xab\xe5\xa4\xb1\xe8\xb4\xa5")) {
                lcd_set_state(LCD_STATE_ASR_ERR);
                lcd_arm_idle_in(3000);
            }
            // 识别 (without 失败) = E8 AF 86 E5 88 AB — bridge sends "识别中…"
            else if (strstr(t, "\xe8\xaf\x86\xe5\x88\xab"))
                lcd_set_state(LCD_STATE_ASR);
            // 请再 = E8 AF B7 E5 86 8D — voice_loop's "请再说一次" (legacy
            // path — we no longer retry, but keep the mapping in case)
            else if (strstr(t, "\xe8\xaf\xb7\xe5\x86\x8d")) {
                lcd_set_state(LCD_STATE_ASR_ERR);
                lcd_arm_idle_in(3000);
            }
            // 没听清 = E6 B2 A1 E5 90 AC E6 B8 85
            else if (strstr(t, "\xe6\xb2\xa1\xe5\x90\xac\xe6\xb8\x85")) {
                lcd_set_state(LCD_STATE_ASR_ERR);
                lcd_arm_idle_in(3000);
            }
            // 没听到 = E6 B2 A1 E5 90 AC E5 88 B0 — bridge said "no audio"
            else if (strstr(t, "\xe6\xb2\xa1\xe5\x90\xac\xe5\x88\xb0"))
                lcd_set_state(LCD_STATE_IDLE);
            else if (strstr(t, "\xe8\xaf\xb7\xe8\xae\xb2") ||
                     strstr(t, "\xe7\xbb\xa7\xe7\xbb\xad")) lcd_set_state(LCD_STATE_LISTENING);
        }
        send_res_ok(rpc_id, nullptr);
    } else if (strcmp(method, "ping") == 0) {
        send_res_ok(rpc_id, nullptr);
    } else if (strcmp(method, "set_led") == 0) {
        const cJSON *p_j = cJSON_GetObjectItemCaseSensitive(root, "p");
        if (!p_j) { send_res_err(rpc_id, "bad_params", "missing p"); return; }
        const cJSON *r_j = cJSON_GetObjectItemCaseSensitive(p_j, "r");
        const cJSON *g_j = cJSON_GetObjectItemCaseSensitive(p_j, "g");
        const cJSON *b_j = cJSON_GetObjectItemCaseSensitive(p_j, "b");
        const cJSON *i_j = cJSON_GetObjectItemCaseSensitive(p_j, "index");
        uint8_t r = cJSON_IsNumber(r_j) ? (uint8_t)r_j->valueint : 0;
        uint8_t g = cJSON_IsNumber(g_j) ? (uint8_t)g_j->valueint : 0;
        uint8_t b = cJSON_IsNumber(b_j) ? (uint8_t)b_j->valueint : 0;
        esp_err_t err;
        if (cJSON_IsNumber(i_j)) {
            err = led_set_pixel((uint8_t)i_j->valueint, r, g, b);
        } else {
            err = led_set_all(r, g, b);
        }
        if (err == ESP_OK) send_res_ok(rpc_id, nullptr);
        else               send_res_err(rpc_id, "led_failed", esp_err_to_name(err));
    } else {
        // set_led / motion / display.image / wake_resume etc. — ack-with-err
        // so the bridge doesn't hang waiting for a response.
        send_res_err(rpc_id, "not_implemented", method);
    }
}

void on_text(const char *data, size_t len) {
    cJSON *root = cJSON_ParseWithLength(data, len);
    if (!root) return;
    const cJSON *t_j = cJSON_GetObjectItemCaseSensitive(root, "t");
    if (cJSON_IsString(t_j)) {
        const char *t = t_j->valuestring;
        if (strcmp(t, "hello.ack") == 0) {
            g_hello_acked = true;
            lcd_set_state(LCD_STATE_IDLE);
            ESP_LOGI(TAG, "← hello.ack — bridge online (LCD=IDLE)");
        } else if (strcmp(t, "req") == 0) {
            handle_req(root);
        } else if (strcmp(t, "tts.start") == 0) {
            const cJSON *sid_j = cJSON_GetObjectItemCaseSensitive(root, "sid");
            uint16_t sid = cJSON_IsNumber(sid_j) ? (uint16_t)sid_j->valueint : 0;
            g_tts_sid = sid;
            g_tts_seq = 0;
            g_tts_in_flight = true;
            speaker_play_start(sid);
            lcd_set_state(LCD_STATE_SPEAKING);
            ESP_LOGI(TAG, "← tts.start sid=%u (LCD=TALK)", (unsigned)sid);
        } else if (strcmp(t, "tts.end") == 0) {
            const cJSON *sid_j = cJSON_GetObjectItemCaseSensitive(root, "sid");
            uint16_t sid = cJSON_IsNumber(sid_j) ? (uint16_t)sid_j->valueint : g_tts_sid;
            speaker_play_end(sid);
            g_tts_in_flight = false;
            ESP_LOGI(TAG, "← tts.end sid=%u", (unsigned)sid);
        } else if (strcmp(t, "ping") == 0) {
            send_text("{\"t\":\"pong\",\"ts\":0}");
        } else {
            ESP_LOGD(TAG, "← unhandled text type: %s", t);
        }
    }
    cJSON_Delete(root);
}

void on_binary(const uint8_t *data, size_t len) {
    if (len < BINARY_HEADER_LEN) {
        ESP_LOGW(TAG, "binary frame too short: %u", (unsigned)len);
        return;
    }
    uint8_t ver  = data[0];
    uint8_t kind = data[1];
    if (ver != PROTOCOL_VERSION) {
        ESP_LOGW(TAG, "binary frame ver mismatch: 0x%02x", ver);
        return;
    }
    if (kind != KIND_TTS_PCM && kind != KIND_TTS_OPUS) {
        ESP_LOGW(TAG, "binary frame unknown kind: 0x%02x", kind);
        return;
    }
    uint32_t seq = (uint32_t)data[4]
                 | ((uint32_t)data[5] << 8)
                 | ((uint32_t)data[6] << 16)
                 | ((uint32_t)data[7] << 24);
    size_t payload_bytes = len - BINARY_HEADER_LEN;
    if (payload_bytes < 1) {
        ESP_LOGW(TAG, "tts pcm bad payload: %u bytes", (unsigned)payload_bytes);
        return;
    }
    if (kind == KIND_TTS_OPUS) {
        speaker_play_opus(data + BINARY_HEADER_LEN, payload_bytes, (uint16_t)seq);
    } else {
        if (payload_bytes < 2 || (payload_bytes & 1)) {
            ESP_LOGW(TAG, "tts pcm bad payload: %u bytes", (unsigned)payload_bytes);
            return;
        }
        const int16_t *pcm = (const int16_t *)(data + BINARY_HEADER_LEN);
        size_t n_samples = payload_bytes / sizeof(int16_t);
        speaker_play_pcm(pcm, n_samples, (uint16_t)seq);
    }
}

void ws_event(void *arg, esp_event_base_t base, int32_t event_id, void *event_data) {
    auto *data = static_cast<esp_websocket_event_data_t *>(event_data);
    switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "ws connected");
        g_connected     = true;
        g_hello_acked   = false;
        g_mic_streaming = false;
        send_hello();
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "ws disconnected");
        g_connected     = false;
        g_hello_acked   = false;
        g_mic_streaming = false;
        // Drop any in-flight audio + flip the screen back to the BRIDGE
        // "(re)connecting" state. Hardware: the LCD panel retains the
        // previous frame until something repaints, so without this the
        // user stares at LISTEN / TALK / THINK forever while we re-handshake.
        speaker_stop();
        lcd_set_state(LCD_STATE_BRIDGE);
        break;
    case WEBSOCKET_EVENT_DATA: {
        // IDF's esp_websocket_client splits a single ws frame into one
        // event per TCP segment (~1436 bytes), each tagged op_code=0x02
        // (not continuation 0x00). payload_offset + data_len < payload_len
        // means there's more coming for this frame. We reassemble here
        // before handing off to on_binary, which expects a complete frame
        // with the 8-byte header at offset 0.
        static uint8_t  reasm[8192];   // covers tts_pcm (1928 B), set_image (large), opus (~70 B)
        static size_t   reasm_filled = 0;
        if (data->op_code == 0x01 /* text */ && data->data_len > 0) {
            on_text((const char *)data->data_ptr, data->data_len);
        } else if (data->op_code == 0x02 /* binary */ && data->data_len > 0) {
            if (data->payload_offset == 0) reasm_filled = 0;
            if (reasm_filled + data->data_len <= sizeof(reasm)) {
                memcpy(reasm + reasm_filled, data->data_ptr, data->data_len);
                reasm_filled += data->data_len;
            } else {
                ESP_LOGW(TAG, "ws reasm overflow: filled=%u + %d > %u — frame discarded",
                         (unsigned)reasm_filled, data->data_len, (unsigned)sizeof(reasm));
                reasm_filled = 0;
                break;
            }
            if ((int)reasm_filled >= data->payload_len) {
                on_binary(reasm, reasm_filled);
                reasm_filled = 0;
            }
        }
        break;
    }
    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGW(TAG, "ws error");
        break;
    default:
        break;
    }
}

}  // namespace

bool bridge_ws_is_connected(void)   { return g_connected && g_hello_acked; }
bool bridge_ws_mic_streaming(void)  { return bridge_ws_is_connected() && g_mic_streaming; }
void bridge_ws_mark_speech_observed(void) { if (g_mic_streaming) g_saw_speech = true; }
bool bridge_ws_saw_speech(void)     { return g_saw_speech; }

esp_err_t bridge_ws_start(const char *bridge_host, int bridge_port,
                           const char *device_id, uint32_t boot_count,
                           const char *fw_version) {
    strncpy(g_device_id,   device_id,   sizeof(g_device_id)   - 1);
    strncpy(g_fw_version,  fw_version,  sizeof(g_fw_version)  - 1);
    g_boot_count = boot_count;

    char uri[160];
    snprintf(uri, sizeof(uri), "ws://%s:%d/", bridge_host, bridge_port);
    ESP_LOGI(TAG, "connecting %s", uri);

    esp_websocket_client_config_t cfg = {};
    cfg.uri                       = uri;
    // Aggressive reconnect: transient WS drops (TCP send-buffer full, brief
    // WiFi roam) shouldn't make the user wait 5 s staring at the BRIDGE
    // screen. 1.5 s gets us back online before the user notices.
    cfg.reconnect_timeout_ms      = 1500;
    cfg.network_timeout_ms        = 10000;
    cfg.buffer_size               = 4096;    // OPUS packets are <200 B; this is plenty
    // Default 4 KB task stack overflows when we mix cJSON + send_bin
    // + event handler in the same task at 31 fps. 8 KB has comfortable
    // margin.
    cfg.task_stack                = 8192;
    cfg.task_prio                 = 5;
    // Don't auto-disconnect on missed pong — wifi RSSI here is -70 dB
    // and we'd rather miss a heartbeat than tear the audio stream.
    // WS keepalive: send a ping frame every 30 s so the path's NAT /
    // session table sees traffic and doesn't evict the connection. WS pings
    // are 2-byte control frames; cheap. Without this the link sat silent
    // for minutes between turns and got dropped by router/AP NAT every
    // 3-5 minutes (matched typical NAT TCP idle timeouts).
    //
    // disable_pingpong_discon=true: ping is for *keepalive*, not health
    // check. We don't want a transient pong loss to kill an otherwise
    // healthy connection — if the WS is truly dead, transport_poll_write
    // failures or TCP RST will surface it within a frame or two.
    cfg.disable_pingpong_discon   = true;
    cfg.ping_interval_sec         = 30;
    g_client = esp_websocket_client_init(&cfg);
    if (!g_client) {
        ESP_LOGE(TAG, "esp_websocket_client_init failed");
        return ESP_FAIL;
    }
    esp_websocket_register_events(g_client, WEBSOCKET_EVENT_ANY, ws_event, nullptr);
    esp_err_t err = esp_websocket_client_start(g_client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_websocket_client_start: %s", esp_err_to_name(err));
        return err;
    }
    return ESP_OK;
}

void bridge_ws_send_mic_pcm(const int16_t *samples, size_t n_samples) {
    if (!bridge_ws_mic_streaming() || !samples || n_samples == 0) return;
    const size_t payload_bytes = n_samples * sizeof(int16_t);
    const size_t total         = BINARY_HEADER_LEN + payload_bytes;
    static uint8_t buf[BINARY_HEADER_LEN + 2048 * sizeof(int16_t)];
    if (total > sizeof(buf)) {
        ESP_LOGW(TAG, "mic frame too large: %u bytes", (unsigned)total);
        return;
    }
    buf[0] = PROTOCOL_VERSION;
    buf[1] = KIND_MIC_PCM;
    buf[2] = (uint8_t)(g_mic_sid & 0xFF);
    buf[3] = (uint8_t)((g_mic_sid >> 8) & 0xFF);
    buf[4] = (uint8_t)(g_mic_seq & 0xFF);
    buf[5] = (uint8_t)((g_mic_seq >> 8) & 0xFF);
    buf[6] = (uint8_t)((g_mic_seq >> 16) & 0xFF);
    buf[7] = (uint8_t)((g_mic_seq >> 24) & 0xFF);
    memcpy(buf + BINARY_HEADER_LEN, samples, payload_bytes);
    int sent = esp_websocket_client_send_bin(
        g_client, (const char *)buf, total, pdMS_TO_TICKS(1000));
    if (sent < 0) {
        ESP_LOGW(TAG, "send_bin seq=%lu failed (%d) — dropping",
                 (unsigned long)g_mic_seq, sent);
        return;
    }
    ++g_mic_seq;
}

void bridge_ws_signal_speech_end(const char *reason) {
    if (!bridge_ws_mic_streaming()) return;
    char buf[96];
    snprintf(buf, sizeof(buf),
             "{\"t\":\"mic.end\",\"sid\":%u,\"reason\":\"%s\",\"total\":%lu}",
             (unsigned)g_mic_sid, reason ? reason : "vad",
             (unsigned long)g_mic_seq);
    send_text(buf);
    g_mic_streaming = false;
    ESP_LOGI(TAG, "→ mic.end sid=%u reason=%s sent=%lu frames",
             (unsigned)g_mic_sid, reason ? reason : "vad",
             (unsigned long)g_mic_seq);
}

void bridge_ws_send_tts_done(uint16_t sid) {
    if (!bridge_ws_is_connected()) return;
    char buf[80];
    snprintf(buf, sizeof(buf),
             "{\"t\":\"evt\",\"name\":\"tts.done\",\"d\":{\"sid\":%u}}",
             (unsigned)sid);
    send_text(buf);
    ESP_LOGI(TAG, "→ evt tts.done sid=%u", (unsigned)sid);
}

void bridge_ws_send_wake_event(const char *word) {
    if (!bridge_ws_is_connected()) return;
    char buf[128];
    snprintf(buf, sizeof(buf),
             "{\"t\":\"evt\",\"name\":\"wake\",\"d\":{\"word\":\"%s\"}}",
             word ? word : "wake");
    send_text(buf);
    ESP_LOGI(TAG, "→ evt wake word=%s", word ? word : "wake");
}

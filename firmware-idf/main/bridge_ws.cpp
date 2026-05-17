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

static const char *TAG = "bridge_ws";

namespace {

// Match bridge/protocol.py exactly.
constexpr uint8_t PROTOCOL_VERSION = 0x02;
constexpr uint8_t KIND_MIC_PCM     = 0x01;

constexpr size_t BINARY_HEADER_LEN = 8;
// 8 bytes: u8 ver, u8 kind, u16 sid LE, u32 seq LE

esp_websocket_client_handle_t g_client = nullptr;
bool g_connected      = false;
bool g_hello_acked    = false;
bool g_mic_streaming  = false;
uint16_t g_mic_sid    = 0;
uint32_t g_mic_seq    = 0;

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
        send_res_ok(rpc_id, nullptr);
        char buf[96];
        snprintf(buf, sizeof(buf),
                 "{\"t\":\"mic.start\",\"sid\":%u,\"sr\":16000,\"fmt\":\"pcm16\"}",
                 (unsigned)sid);
        send_text(buf);
        // Loud serial banner so the user has a clear cue to speak (the
        // LCD `show_text` isn't wired up yet in this firmware).
        ESP_LOGW(TAG, "");
        ESP_LOGW(TAG, "==========================================");
        ESP_LOGW(TAG, "  🎤 MIC OPEN — TALK NOW  (sid=%u)", (unsigned)sid);
        ESP_LOGW(TAG, "==========================================");
    } else if (strcmp(method, "mic_stop") == 0) {
        g_mic_streaming = false;
        send_res_ok(rpc_id, nullptr);
        ESP_LOGI(TAG, "mic streaming off (server stop)");
    } else if (strcmp(method, "ping") == 0) {
        send_res_ok(rpc_id, nullptr);
    } else {
        // Unknown methods are common in Phase 3 (tts_start, show_text, set_led
        // etc.) — ack-with-err so the bridge doesn't hang.
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
            ESP_LOGI(TAG, "← hello.ack — bridge online");
        } else if (strcmp(t, "req") == 0) {
            handle_req(root);
        } else if (strcmp(t, "ping") == 0) {
            // bridge sends ping; reply pong
            send_text("{\"t\":\"pong\",\"ts\":0}");
        } else {
            ESP_LOGD(TAG, "← unhandled text type: %s", t);
        }
    }
    cJSON_Delete(root);
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
        break;
    case WEBSOCKET_EVENT_DATA:
        if (data->op_code == 0x01 /* text */ && data->data_len > 0) {
            on_text((const char *)data->data_ptr, data->data_len);
        }
        break;
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
    cfg.reconnect_timeout_ms      = 5000;
    cfg.network_timeout_ms        = 10000;
    cfg.buffer_size               = 4096;
    // Default 4 KB task stack overflows when we mix cJSON + send_bin
    // + event handler in the same task at 31 fps. 8 KB has comfortable
    // margin.
    cfg.task_stack                = 8192;
    cfg.task_prio                 = 5;
    // Don't auto-disconnect on missed pong — wifi RSSI here is -70 dB
    // and we'd rather miss a heartbeat than tear the audio stream.
    cfg.disable_pingpong_discon   = true;
    cfg.ping_interval_sec         = 0;     // disable client-side pings
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
        g_client, (const char *)buf, total, pdMS_TO_TICKS(200));
    if (sent < 0) {
        ESP_LOGW(TAG, "send_bin failed (%d)", sent);
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

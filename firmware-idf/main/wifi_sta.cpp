// WiFi STA bringup — minimal. Built directly on the IDF event loop
// instead of pulling in `esp-wifi-connect` to keep dependencies tight.

#include "wifi_sta.h"

#include <string.h>

#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

static const char *TAG = "wifi";

namespace {

constexpr int kMaxAttempts = 8;

constexpr EventBits_t BIT_CONNECTED = BIT0;
constexpr EventBits_t BIT_FAILED    = BIT1;

EventGroupHandle_t g_events = nullptr;
int                g_attempt = 0;

void event_handler(void *arg, esp_event_base_t base, int32_t id, void *data) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (g_attempt < kMaxAttempts) {
            ++g_attempt;
            ESP_LOGW(TAG, "disconnected, retry %d/%d", g_attempt, kMaxAttempts);
            esp_wifi_connect();
        } else {
            xEventGroupSetBits(g_events, BIT_FAILED);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        auto *evt = static_cast<ip_event_got_ip_t *>(data);
        ESP_LOGI(TAG, "got ip: " IPSTR, IP2STR(&evt->ip_info.ip));
        g_attempt = 0;
        xEventGroupSetBits(g_events, BIT_CONNECTED);
    }
}

}  // namespace

esp_err_t wifi_sta_connect(const char *ssid, const char *password) {
    if (!ssid || !ssid[0]) {
        ESP_LOGE(TAG, "SSID empty — set STACKCHAN_WIFI_SSID in include/config.h");
        return ESP_ERR_INVALID_ARG;
    }
    g_events = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, nullptr, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, nullptr, nullptr));

    wifi_config_t cfg = {};
    strncpy(reinterpret_cast<char *>(cfg.sta.ssid),     ssid,     sizeof(cfg.sta.ssid) - 1);
    strncpy(reinterpret_cast<char *>(cfg.sta.password), password, sizeof(cfg.sta.password) - 1);
    cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    cfg.sta.pmf_cfg.capable    = true;
    cfg.sta.pmf_cfg.required   = false;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "connecting to '%s'…", ssid);
    EventBits_t bits = xEventGroupWaitBits(
        g_events, BIT_CONNECTED | BIT_FAILED, pdFALSE, pdFALSE, pdMS_TO_TICKS(30000));
    if (bits & BIT_CONNECTED) return ESP_OK;
    ESP_LOGE(TAG, "wifi connect failed (bits=0x%lx)", (unsigned long)bits);
    return ESP_FAIL;
}

// audio_udp.cpp — see audio_udp.h header comment for design.
#include "audio_udp.h"

#include <string.h>
#include <sys/socket.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <errno.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "audio_udp";

namespace {

// Match bridge/protocol.py exactly.
constexpr uint8_t PROTOCOL_VERSION = 0x02;
constexpr uint8_t KIND_MIC_PCM     = 0x01;
constexpr uint8_t KIND_UDP_HELLO   = 0x80;
constexpr uint8_t KIND_UDP_ACK     = 0x81;
constexpr uint8_t KIND_UDP_KA      = 0x82;

constexpr size_t  BINARY_HEADER_LEN = 8;
constexpr int     KEEPALIVE_INTERVAL_MS = 15000;
constexpr int64_t HEALTH_TIMEOUT_US     = 30LL * 1000 * 1000;  // 30 s
constexpr int64_t REPROBE_INTERVAL_US   = 5LL * 1000 * 1000;   // 5 s while unhealthy

int                 g_socket          = -1;
struct sockaddr_in  g_bridge_sin{};
char                g_device_id[64]   = {0};
volatile bool       g_healthy         = false;
volatile int64_t    g_last_rx_us      = 0;
volatile int64_t    g_last_probe_us   = 0;
TaskHandle_t        g_recv_task_h     = nullptr;
TaskHandle_t        g_ka_task_h       = nullptr;

void pack_header(uint8_t *buf, uint8_t kind, uint16_t sid, uint32_t seq) {
    buf[0] = PROTOCOL_VERSION;
    buf[1] = kind;
    buf[2] = (uint8_t)(sid & 0xFF);
    buf[3] = (uint8_t)((sid >> 8) & 0xFF);
    buf[4] = (uint8_t)(seq & 0xFF);
    buf[5] = (uint8_t)((seq >> 8) & 0xFF);
    buf[6] = (uint8_t)((seq >> 16) & 0xFF);
    buf[7] = (uint8_t)((seq >> 24) & 0xFF);
}

void send_to_bridge(uint8_t kind, const uint8_t *payload, size_t payload_len) {
    if (g_socket < 0) return;
    uint8_t header[BINARY_HEADER_LEN];
    pack_header(header, kind, 0, 0);
    if (payload_len == 0) {
        ssize_t n = sendto(g_socket, header, BINARY_HEADER_LEN, MSG_DONTWAIT,
                           (struct sockaddr *)&g_bridge_sin, sizeof(g_bridge_sin));
        if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            ESP_LOGW(TAG, "sendto kind=0x%02x failed: errno=%d", kind, errno);
        }
        return;
    }
    // Two-iov style — combine in a stack buffer to avoid sendmsg complexity.
    // Largest UDP_HELLO payload is device_id (~32 B). Mic_pcm uses a
    // dedicated path with caller-provided contiguous buffer.
    uint8_t small[BINARY_HEADER_LEN + 96];
    if (BINARY_HEADER_LEN + payload_len > sizeof(small)) {
        ESP_LOGW(TAG, "control payload too large: %u B", (unsigned)payload_len);
        return;
    }
    memcpy(small, header, BINARY_HEADER_LEN);
    memcpy(small + BINARY_HEADER_LEN, payload, payload_len);
    ssize_t n = sendto(g_socket, small, BINARY_HEADER_LEN + payload_len, MSG_DONTWAIT,
                       (struct sockaddr *)&g_bridge_sin, sizeof(g_bridge_sin));
    if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
        ESP_LOGW(TAG, "sendto kind=0x%02x %u B failed: errno=%d",
                 kind, (unsigned)payload_len, errno);
    }
}

void send_probe(void) {
    g_last_probe_us = esp_timer_get_time();
    size_t id_len = strlen(g_device_id);
    send_to_bridge(KIND_UDP_HELLO, (const uint8_t *)g_device_id, id_len);
    ESP_LOGI(TAG, "→ udp.hello %s", g_device_id);
}

void recv_task(void *) {
    uint8_t buf[256];
    fd_set rfds;
    while (true) {
        FD_ZERO(&rfds);
        FD_SET(g_socket, &rfds);
        struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
        int r = select(g_socket + 1, &rfds, nullptr, nullptr, &tv);
        if (r < 0) {
            if (errno == EINTR) continue;
            ESP_LOGW(TAG, "select errno=%d", errno);
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        if (r > 0 && FD_ISSET(g_socket, &rfds)) {
            struct sockaddr_in from{};
            socklen_t flen = sizeof(from);
            ssize_t n = recvfrom(g_socket, buf, sizeof(buf), 0,
                                 (struct sockaddr *)&from, &flen);
            if (n < (ssize_t)BINARY_HEADER_LEN) continue;
            if (buf[0] != PROTOCOL_VERSION) continue;
            uint8_t kind = buf[1];
            g_last_rx_us = esp_timer_get_time();
            if (kind == KIND_UDP_ACK) {
                if (!g_healthy) {
                    ESP_LOGI(TAG, "← udp.ack — audio path UDP healthy");
                }
                g_healthy = true;
            } else if (kind == KIND_UDP_KA) {
                // Just a keepalive; last_rx_us already updated above.
            }
            // mic_pcm / tts_opus / etc are not expected on device-rx
            // side in v1 (TTS still over WS). Ignore for now.
        }
        // Health watchdog runs on every wake (≥ once a second).
        int64_t now = esp_timer_get_time();
        if (g_healthy && (now - g_last_rx_us) > HEALTH_TIMEOUT_US) {
            ESP_LOGW(TAG, "udp silent for %lld ms — marking unhealthy",
                     (long long)((now - g_last_rx_us) / 1000));
            g_healthy = false;
        }
        if (!g_healthy && (now - g_last_probe_us) > REPROBE_INTERVAL_US) {
            send_probe();
        }
    }
}

void keepalive_task(void *) {
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(KEEPALIVE_INTERVAL_MS));
        if (g_healthy) {
            send_to_bridge(KIND_UDP_KA, nullptr, 0);
        }
    }
}

}  // namespace

esp_err_t audio_udp_start(const char *bridge_host,
                          int bridge_port,
                          const char *device_id) {
    if (!bridge_host || !device_id) return ESP_ERR_INVALID_ARG;
    if (g_socket >= 0) return ESP_OK;   // idempotent

    strncpy(g_device_id, device_id, sizeof(g_device_id) - 1);

    // Resolve hostname → IPv4. Bridge runs on a fixed LAN IP in
    // practice (192.168.x.y), so getaddrinfo is sync + fast.
    struct addrinfo hints{};
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    struct addrinfo *res = nullptr;
    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%d", bridge_port);
    int gai = getaddrinfo(bridge_host, port_str, &hints, &res);
    if (gai != 0 || !res) {
        ESP_LOGE(TAG, "getaddrinfo %s:%d failed: %d", bridge_host, bridge_port, gai);
        return ESP_FAIL;
    }
    memcpy(&g_bridge_sin, res->ai_addr, sizeof(g_bridge_sin));
    freeaddrinfo(res);

    g_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (g_socket < 0) {
        ESP_LOGE(TAG, "socket() errno=%d", errno);
        return ESP_FAIL;
    }
    // Bind ephemeral source port.
    struct sockaddr_in local{};
    local.sin_family      = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    local.sin_port        = 0;
    if (bind(g_socket, (struct sockaddr *)&local, sizeof(local)) < 0) {
        ESP_LOGE(TAG, "bind errno=%d", errno);
        close(g_socket);
        g_socket = -1;
        return ESP_FAIL;
    }
    socklen_t llen = sizeof(local);
    getsockname(g_socket, (struct sockaddr *)&local, &llen);
    ESP_LOGI(TAG, "udp socket bound to local port %u — bridge=%s:%d",
             (unsigned)ntohs(local.sin_port), bridge_host, bridge_port);

    BaseType_t ok;
    ok = xTaskCreatePinnedToCore(recv_task, "udp_rx", 4096, nullptr, 5,
                                  &g_recv_task_h, tskNO_AFFINITY);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "recv_task create failed");
        close(g_socket); g_socket = -1;
        return ESP_FAIL;
    }
    ok = xTaskCreatePinnedToCore(keepalive_task, "udp_ka", 2048, nullptr, 4,
                                  &g_ka_task_h, tskNO_AFFINITY);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "keepalive_task create failed");
        // recv_task continues, harmless.
    }
    return ESP_OK;
}

void audio_udp_kick_probe(void) {
    if (g_socket < 0) return;
    // Defensive: clear the local "healthy" flag so we don't send mic
    // frames to a bridge that has popped our Device entry (e.g. just
    // after a WS reconnect). The next ACK will re-arm us within ~1 ms
    // on LAN; until then the WS fallback in bridge_ws_send_mic_pcm
    // carries the audio.
    g_healthy = false;
    send_probe();
}

bool audio_udp_is_healthy(void) {
    return g_healthy;
}

bool audio_udp_send_mic_pcm(const int16_t *pcm16,
                            size_t n_samples,
                            uint16_t sid,
                            uint32_t seq) {
    if (!g_healthy || g_socket < 0 || !pcm16 || n_samples == 0) return false;
    const size_t payload_bytes = n_samples * sizeof(int16_t);
    const size_t total         = BINARY_HEADER_LEN + payload_bytes;
    // Dedicated stack buffer — main mic frame is 512 samples = 1024 B
    // payload + 8 B header = 1032 B << MTU (~1500). One sendto = one
    // UDP datagram, no IP fragmentation.
    static uint8_t buf[BINARY_HEADER_LEN + 2048 * sizeof(int16_t)];
    if (total > sizeof(buf)) {
        ESP_LOGW(TAG, "mic frame too large: %u bytes", (unsigned)total);
        return false;
    }
    pack_header(buf, KIND_MIC_PCM, sid, seq);
    memcpy(buf + BINARY_HEADER_LEN, pcm16, payload_bytes);
    ssize_t n = sendto(g_socket, buf, total, MSG_DONTWAIT,
                       (struct sockaddr *)&g_bridge_sin, sizeof(g_bridge_sin));
    if (n < 0) {
        // EAGAIN here would be unusual on UDP (no flow control); only
        // happens on a transient WiFi/lwIP queue burst. Drop frame.
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            ESP_LOGW(TAG, "mic sendto errno=%d", errno);
        }
        return false;
    }
    return true;
}

// Minimal WiFi station bringup for the IDF spike.
//
// Connects (blocking, with retry) and returns when an IP is granted.
// Reuses ../../include/config.h so the Arduino + IDF firmwares share
// one place to edit SSID/password.

#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Blocks until connected (or hard-fails after kMaxAttempts retries).
// Logs the obtained IPv4 address on success.
esp_err_t wifi_sta_connect(const char *ssid, const char *password);

#ifdef __cplusplus
}
#endif

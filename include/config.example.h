#pragma once

// Copy this file to include/config.h and fill in your 2.4GHz Wi-Fi details.
// If STACKCHAN_WIFI_SSID is empty, the firmware starts an AP named
// StackChan-OpenClaw instead.
#define STACKCHAN_WIFI_SSID ""
#define STACKCHAN_WIFI_PASSWORD ""

// Optional. Leave empty for no HTTP API auth. If set, requests must include:
//   X-StackChan-Token: your-token
#define STACKCHAN_TOKEN ""

// Shown on the boot/status screen only. The firmware does not need to call
// OpenClaw directly; OpenClaw controls this device through the HTTP API.
#define OPENCLAW_GATEWAY_URL ""

// Bridge daemon WebSocket URL. The device opens a persistent WS connection to
// this host:port for the ocsc.v2 sub-protocol (RPC + streaming audio).
// Format: "host:port" (no scheme, no path; the firmware always uses ws:// + /).
// Leave empty to disable the WS client entirely (HTTP API still works).
#define STACKPROXY_WS_HOST "192.168.1.10"
#define STACKPROXY_WS_PORT 8765

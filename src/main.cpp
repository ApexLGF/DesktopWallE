#include <Arduino.h>
#include <ArduinoJson.h>
#include <ESPmDNS.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <WebServer.h>
#include <WebSocketsClient.h>
#include <WiFi.h>
#include <M5StackChan.h>
#include <esp_camera.h>
#include <esp_random.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#if __has_include("../include/config.h")
#include "../include/config.h"
#elif __has_include("config.h")
#include "config.h"
#else
#define STACKCHAN_WIFI_SSID ""
#define STACKCHAN_WIFI_PASSWORD ""
#define STACKCHAN_TOKEN ""
#define OPENCLAW_GATEWAY_URL ""
#endif

#ifndef STACKPROXY_WS_HOST
#define STACKPROXY_WS_HOST ""
#endif
#ifndef STACKPROXY_WS_PORT
#define STACKPROXY_WS_PORT 8765
#endif

namespace {

// =========================================================================
// Constants
// =========================================================================

WebServer server(80);

constexpr const char* kHostName = "stackchan";
constexpr const char* kFirmwareVersion = "0.5.0";

// GC0308 camera pin map for M5Stack CoreS3 (verified against
// GOB52/M5StackCoreS3_CameraWebServer and M5Stack official examples).
constexpr int kCamPinPwdn  = -1;
constexpr int kCamPinReset = -1;
constexpr int kCamPinXclk  = 2;
constexpr int kCamPinSiod  = 12;   // I2C SDA — shared with M5.In_I2C
constexpr int kCamPinSioc  = 11;   // I2C SCL — shared with M5.In_I2C
constexpr int kCamPinD7    = 47;
constexpr int kCamPinD6    = 48;
constexpr int kCamPinD5    = 16;
constexpr int kCamPinD4    = 15;
constexpr int kCamPinD3    = 42;
constexpr int kCamPinD2    = 41;
constexpr int kCamPinD1    = 40;
constexpr int kCamPinD0    = 39;
constexpr int kCamPinVsync = 46;
constexpr int kCamPinHref  = 38;
constexpr int kCamPinPclk  = 45;

constexpr int kHeadXMin = -1280;
constexpr int kHeadXMax = 1280;
constexpr int kHeadYMin = 0;
constexpr int kHeadYMax = 850;
constexpr int kHeadYHomeMin = 50;
constexpr int kSpeedMin = 0;
constexpr int kSpeedMax = 1000;
constexpr int kLedCount = 12;

constexpr int kContentTop = 56;
constexpr int kContentBottom = 224;
constexpr int kLineHeight = 16;
constexpr int kRowsPerPage = (kContentBottom - kContentTop) / kLineHeight;
constexpr int kSideMargin = 10;
constexpr uint32_t kPageIntervalMs = 3500;

constexpr size_t kSpeakChunkBytes = 4096;
constexpr uint32_t kSpeakDefaultSampleRate = 16000;
constexpr int kSpeakQueueLen = 2;
constexpr int kSpeakHttpTimeoutMs = 8000;
constexpr size_t kSpeakMaxBytes = 4 * 1024 * 1024;

constexpr uint32_t kMicMaxSeconds = 10;
constexpr uint32_t kMicDefaultSampleRate = 16000;

constexpr uint32_t kEventCapacity = 64;
constexpr uint32_t kSensorPollIntervalMs = 25;
constexpr uint32_t kIdleMinIntervalMs = 4000;
constexpr uint32_t kIdleMaxIntervalMs = 9000;

// =========================================================================
// State
// =========================================================================

String lastText = "Ready for OpenClaw";
String lastTitle = "StackChan";
int currentX = 0;
int currentY = 0;

enum class DisplayMode : uint8_t {
    Status,
    Message,
    Face,
    Big,
    Clock,
    Marquee,
    Progress,
    Clear,
};
DisplayMode displayMode = DisplayMode::Status;
uint32_t displayNeedsRedraw = 0;

std::vector<String> messageLines;
int currentPage = 0;
int totalPages = 1;
uint32_t lastPageMillis = 0;

// Face rendering
String faceExpression = "neutral";
uint32_t faceEyeColor = 0x00CED1;     // cyan
uint32_t faceMouthColor = 0xFFA0B4;   // pink
uint32_t faceBgColor = 0x000000;
uint32_t faceSkinColor = 0x101820;
uint32_t lastFaceFrameMs = 0;
uint32_t lastBlinkMs = 0;
uint32_t blinkUntilMs = 0;
bool blinking = false;

// Big text
String bigText;
uint32_t bigColor = 0xFFFFFF;

// Marquee
String marqueeText;
int marqueePixelOffset = 0;
uint32_t lastMarqueeMs = 0;
uint16_t marqueeStepMs = 40;
uint32_t marqueeColor = 0xFFFFFF;

// Progress
int progressPercent = 0;
String progressLabel = "";

// LED effect engine
enum class LedEffect : uint8_t {
    Off,
    Solid,
    Rainbow,
    Breathing,
    Pulse,
    Scanner,
    Wipe,
    Sparkle,
    Police,
    Fire,
    Chase,
    Theater,
    Listening,
    Thinking,
    Talking,
    Recording,   // fixed-pattern red blink to make "mic on" unmistakable
};
LedEffect ledEffect = LedEffect::Solid;
uint8_t ledR = 0, ledG = 24, ledB = 48;
uint16_t ledSpeed = 80;     // ms per frame
uint32_t lastLedFrameMs = 0;
uint32_t ledFrame = 0;

// Idle motion
bool idleMotionEnabled = false;
uint32_t nextIdleMs = 0;
bool autoBreathingEnabled = false;
uint32_t lastBreathMs = 0;
int breathPhase = 0;

// Sensors / events
uint32_t lastSensorPollMs = 0;
float lastAccelX = 0, lastAccelY = 0, lastAccelZ = 1.0f;
float lastGyroX = 0, lastGyroY = 0, lastGyroZ = 0;
float lastMagX = 0, lastMagY = 0, lastMagZ = 0;
bool hasMagnetometer = false;
float lastTempC = 0;
float shakeIntegrator = 0;
uint32_t lastShakeEventMs = 0;
uint32_t lastTouchEventMs = 0;

// Camera
bool cameraEnabled = false;
bool cameraInitTried = false;
String cameraInitError;
framesize_t cameraFrameSize = FRAMESIZE_QVGA;  // 320x240 default — fast to JPEG-encode
int cameraQuality = 12;                        // 0-63, lower = better
uint32_t cameraLastCaptureMs = 0;
uint32_t cameraLastBytes = 0;
SemaphoreHandle_t cameraMutex = nullptr;
// Camera job handoff — ALL esp_camera access is serialized onto cameraTask so the
// WebServer / WS tasks never touch the camera directly. The old code did
// esp_camera_fb_get + software JPEG encode + esp_camera_deinit on the WebServer
// task on every shot; that teardown raced the WS task and M5StackChan.update() on
// the shared I2C bus and hard-wedged the whole device. Now: init once, never
// deinit, capture + encode on a dedicated task.
struct CameraJob {
    uint8_t* jpg = nullptr;   // OUT: heap buffer; the requester must free() it
    size_t   jpgLen = 0;      // OUT
    uint32_t captureMs = 0;   // OUT: capture + encode duration
    bool     ok = false;      // OUT
    char     err[40] = {0};   // OUT: error string on failure
};
TaskHandle_t cameraTaskHandle = nullptr;
SemaphoreHandle_t cameraJobDone = nullptr;  // cameraTask gives it when result ready
CameraJob cameraJob;                        // shared; guarded by cameraMutex

struct Event {
    uint32_t id;
    uint32_t ts;
    char type[32];
    char data[128];
};
Event eventRing[kEventCapacity];
uint32_t eventHead = 0;
uint32_t eventCount = 0;
uint32_t nextEventId = 1;
SemaphoreHandle_t eventMutex = nullptr;

// Speak (existing)
struct SpeakRequest {
    char url[256];
    char title[96];
    uint32_t id;
    bool hasTitle;
};
QueueHandle_t speakQueue = nullptr;
SemaphoreHandle_t speakStateMutex = nullptr;
TaskHandle_t speakTaskHandle = nullptr;
volatile bool speakStopRequested = false;
bool speakPlaying = false;
uint32_t speakPlayingId = 0;
String speakPlayingUrl;
String speakPlayingTitle;
uint32_t speakNextId = 1;

// Mic
struct MicRequest {
    uint32_t id;
    uint32_t seconds;
    uint32_t sample_rate;
    bool upload;
    char upload_url[200];
    char prompt[120];
};
QueueHandle_t micQueue = nullptr;
TaskHandle_t micTaskHandle = nullptr;
SemaphoreHandle_t micStateMutex = nullptr;
bool micRecording = false;
uint32_t micRecordingId = 0;
uint32_t micNextId = 1;
volatile bool micStopRequested = false;

// Most recent mic recording, stored in PSRAM (or DRAM) so /mic/last can return it
uint8_t* micLastWav = nullptr;
size_t micLastWavLen = 0;
String micLastNote;
SemaphoreHandle_t micBufMutex = nullptr;

// Webhook
struct WebhookMsg {
    char url[200];
    char body[384];
};
QueueHandle_t webhookQueue = nullptr;
TaskHandle_t webhookTaskHandle = nullptr;
SemaphoreHandle_t webhookCfgMutex = nullptr;
String webhookUrl;

// WebSocket client (stackproxy long-connection)
WebSocketsClient wsClient;
bool wsBound = false;          // begin() has been called
bool wsConnected = false;
bool wsHelloSent = false;
uint32_t bootCount = 0;
String connectionId;
String deviceId;               // "stackchan-<mac>"
// Outbound queue: events/responses must be sent on the main loop, not from
// arbitrary FreeRTOS tasks (the WebSockets library is not thread-safe).
struct WsOutMsg {
    char buf[512];
};
struct WsOutBin {
    uint8_t* data;     // heap-allocated; freed after send
    size_t length;
};
QueueHandle_t wsOutQueue = nullptr;
QueueHandle_t wsOutBinQueue = nullptr;
constexpr int kWsOutQueueLen = 16;
constexpr int kWsOutBinQueueLen = 32;   // ~3s of mic stream at 100ms/chunk
constexpr const char* kNvsNamespace = "stackchan";
constexpr const char* kNvsKeyBoot = "boot_count";

// Mic stream state (PR5)
TaskHandle_t micStreamTaskHandle = nullptr;
volatile bool micStreaming = false;
volatile bool micStreamStopRequested = false;
uint16_t micStreamSid = 0;
uint32_t micStreamStartMs = 0;
// Watchdog: once mic.end is sent we expect a reply (tts.start or
// agent.error). The OpenClaw agent legitimately takes 15-90s (ReAct loop +
// model latency), so the timeout must be generous — it only exists to
// recover the UI if the agent truly hangs / stackproxy dies. It is re-armed
// fresh when asr.final arrives (that's when the agent actually starts).
volatile uint32_t convoWatchdogDeadlineMs = 0;
constexpr uint32_t kConvoWatchdogMs = 120000;   // 2 min hard ceiling

// =========================================================================
// Forward declarations (resolves ordering between ws_client and pushEvent)
// =========================================================================

void wsSendEvt(const char* name, const char* data);
void micStreamToggle();
void micStreamStart();
void micStreamStop();
void convoResetIdle(const char* title, const char* text);
void convoCancelWatchdog();
void convoBeep(uint16_t freq, uint32_t ms);

// =========================================================================
// Tiny helpers
// =========================================================================

int clampInt(int value, int minValue, int maxValue)
{
    if (value < minValue) return minValue;
    if (value > maxValue) return maxValue;
    return value;
}

float clampFloat(float v, float lo, float hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

uint16_t to565(uint32_t rgb)
{
    const uint8_t r = (rgb >> 16) & 0xFF;
    const uint8_t g = (rgb >> 8) & 0xFF;
    const uint8_t b = rgb & 0xFF;
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

uint32_t parseHexColor(const String& s, uint32_t fallback)
{
    if (s.length() == 0) return fallback;
    const char* p = s.c_str();
    if (*p == '#') p++;
    char* end = nullptr;
    unsigned long v = strtoul(p, &end, 16);
    if (end == nullptr || *end != '\0') return fallback;
    return static_cast<uint32_t>(v);
}

bool tokenConfigured()
{
    return String(STACKCHAN_TOKEN).length() > 0;
}

bool isAuthorized()
{
    if (!tokenConfigured()) return true;
    return server.header("X-StackChan-Token") == String(STACKCHAN_TOKEN);
}

void sendCors()
{
    server.sendHeader("Access-Control-Allow-Origin", "*");
    server.sendHeader("Access-Control-Allow-Headers", "Content-Type, X-StackChan-Token");
    server.sendHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
}

void sendJson(int status, const JsonDocument& doc)
{
    String body;
    serializeJson(doc, body);
    sendCors();
    server.send(status, "application/json", body);
}

void sendOk(const char* message = "ok")
{
    StaticJsonDocument<160> doc;
    doc["ok"] = true;
    doc["message"] = message;
    sendJson(200, doc);
}

void sendError(int status, const char* message)
{
    StaticJsonDocument<200> doc;
    doc["ok"] = false;
    doc["error"] = message;
    sendJson(status, doc);
}

bool requireAuth()
{
    if (isAuthorized()) return true;
    sendError(401, "missing or invalid X-StackChan-Token");
    return false;
}

bool parseBody(JsonDocument& doc, bool allowEmpty = false)
{
    const String body = server.arg("plain");
    if (body.length() == 0) {
        if (allowEmpty) {
            doc.to<JsonObject>();
            return true;
        }
        sendError(400, "missing json body");
        return false;
    }
    DeserializationError err = deserializeJson(doc, body);
    if (err) {
        sendError(400, "invalid json");
        return false;
    }
    return true;
}

int utf8CharBytes(uint8_t lead)
{
    if ((lead & 0x80) == 0) return 1;
    if ((lead & 0xE0) == 0xC0) return 2;
    if ((lead & 0xF0) == 0xE0) return 3;
    if ((lead & 0xF8) == 0xF0) return 4;
    return 1;
}

uint32_t readUint32LE(const uint8_t* p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

uint16_t readUint16LE(const uint8_t* p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

void writeUint32LE(uint8_t* p, uint32_t v)
{
    p[0] = v & 0xFF;
    p[1] = (v >> 8) & 0xFF;
    p[2] = (v >> 16) & 0xFF;
    p[3] = (v >> 24) & 0xFF;
}

void writeUint16LE(uint8_t* p, uint16_t v)
{
    p[0] = v & 0xFF;
    p[1] = (v >> 8) & 0xFF;
}

// =========================================================================
// Event queue
// =========================================================================

void pushEvent(const char* type, const char* data = nullptr)
{
    if (eventMutex == nullptr) return;
    if (xSemaphoreTake(eventMutex, pdMS_TO_TICKS(20)) != pdTRUE) return;
    Event& slot = eventRing[(eventHead + eventCount) % kEventCapacity];
    if (eventCount < kEventCapacity) {
        eventCount++;
    } else {
        eventHead = (eventHead + 1) % kEventCapacity;
    }
    slot.id = nextEventId++;
    slot.ts = millis();
    strlcpy(slot.type, type, sizeof(slot.type));
    if (data) {
        strlcpy(slot.data, data, sizeof(slot.data));
    } else {
        slot.data[0] = '\0';
    }
    xSemaphoreGive(eventMutex);

    // Also dispatch to ws stackproxy as `evt` frame (best-effort, drops if
    // queue full or not yet connected)
    wsSendEvt(type, data);

    // Fire webhook if configured
    if (webhookQueue == nullptr) return;
    String url;
    if (webhookCfgMutex && xSemaphoreTake(webhookCfgMutex, pdMS_TO_TICKS(5)) == pdTRUE) {
        url = webhookUrl;
        xSemaphoreGive(webhookCfgMutex);
    }
    if (url.length() == 0) return;
    WebhookMsg msg = {};
    strlcpy(msg.url, url.c_str(), sizeof(msg.url));
    StaticJsonDocument<256> doc;
    doc["id"] = slot.id;
    doc["ts"] = slot.ts;
    doc["type"] = type;
    if (data) doc["data"] = data;
    serializeJson(doc, msg.body, sizeof(msg.body));
    xQueueSend(webhookQueue, &msg, 0);
}

// =========================================================================
// LED rendering
// =========================================================================

void setLedRaw(int i, uint8_t r, uint8_t g, uint8_t b)
{
    M5StackChan.setRgbColor(static_cast<uint8_t>(i), r, g, b);
}

void refreshLeds()
{
    M5StackChan.refreshRgb();
}

void setAllLeds(uint8_t r, uint8_t g, uint8_t b)
{
    for (int i = 0; i < kLedCount; i++) {
        setLedRaw(i, r, g, b);
    }
    refreshLeds();
}

void hsvToRgb(float h, float s, float v, uint8_t& r, uint8_t& g, uint8_t& b)
{
    h = fmodf(h, 360.0f);
    if (h < 0) h += 360.0f;
    const float c = v * s;
    const float x = c * (1.0f - fabsf(fmodf(h / 60.0f, 2.0f) - 1.0f));
    const float m = v - c;
    float rf, gf, bf;
    if (h < 60)        { rf = c; gf = x; bf = 0; }
    else if (h < 120)  { rf = x; gf = c; bf = 0; }
    else if (h < 180)  { rf = 0; gf = c; bf = x; }
    else if (h < 240)  { rf = 0; gf = x; bf = c; }
    else if (h < 300)  { rf = x; gf = 0; bf = c; }
    else               { rf = c; gf = 0; bf = x; }
    r = static_cast<uint8_t>(clampFloat((rf + m) * 255, 0, 255));
    g = static_cast<uint8_t>(clampFloat((gf + m) * 255, 0, 255));
    b = static_cast<uint8_t>(clampFloat((bf + m) * 255, 0, 255));
}

void ledTickEffect()
{
    switch (ledEffect) {
        case LedEffect::Off: {
            for (int i = 0; i < kLedCount; i++) setLedRaw(i, 0, 0, 0);
            break;
        }
        case LedEffect::Solid: {
            for (int i = 0; i < kLedCount; i++) setLedRaw(i, ledR, ledG, ledB);
            break;
        }
        case LedEffect::Rainbow: {
            for (int i = 0; i < kLedCount; i++) {
                const float h = fmodf((float)ledFrame * 6.0f + (float)i * (360.0f / kLedCount), 360.0f);
                uint8_t r, g, b;
                hsvToRgb(h, 1.0f, 1.0f, r, g, b);
                setLedRaw(i, r, g, b);
            }
            break;
        }
        case LedEffect::Breathing: {
            const float t = fmodf((float)ledFrame * 0.05f, 6.28318f);
            const float amp = (sinf(t) + 1.0f) * 0.5f;
            const uint8_t rr = static_cast<uint8_t>(ledR * amp);
            const uint8_t gg = static_cast<uint8_t>(ledG * amp);
            const uint8_t bb = static_cast<uint8_t>(ledB * amp);
            for (int i = 0; i < kLedCount; i++) setLedRaw(i, rr, gg, bb);
            break;
        }
        case LedEffect::Pulse: {
            const float t = fmodf((float)ledFrame * 0.15f, 2.0f);
            const float amp = (t < 1.0f) ? t : (2.0f - t);
            const uint8_t rr = static_cast<uint8_t>(ledR * amp);
            const uint8_t gg = static_cast<uint8_t>(ledG * amp);
            const uint8_t bb = static_cast<uint8_t>(ledB * amp);
            for (int i = 0; i < kLedCount; i++) setLedRaw(i, rr, gg, bb);
            break;
        }
        case LedEffect::Scanner: {
            const int pos = ledFrame % (2 * kLedCount - 2);
            const int idx = (pos < kLedCount) ? pos : (2 * kLedCount - 2 - pos);
            for (int i = 0; i < kLedCount; i++) {
                const int d = std::abs(i - idx);
                if (d == 0) setLedRaw(i, ledR, ledG, ledB);
                else if (d == 1) setLedRaw(i, ledR / 4, ledG / 4, ledB / 4);
                else setLedRaw(i, 0, 0, 0);
            }
            break;
        }
        case LedEffect::Wipe: {
            const int pos = ledFrame % (kLedCount + 4);
            for (int i = 0; i < kLedCount; i++) {
                if (i <= pos && pos < kLedCount) setLedRaw(i, ledR, ledG, ledB);
                else setLedRaw(i, 0, 0, 0);
            }
            break;
        }
        case LedEffect::Sparkle: {
            for (int i = 0; i < kLedCount; i++) {
                if ((esp_random() & 0x1F) == 0) setLedRaw(i, ledR, ledG, ledB);
                else setLedRaw(i, ledR / 16, ledG / 16, ledB / 16);
            }
            break;
        }
        case LedEffect::Police: {
            const bool red = (ledFrame / 4) % 2 == 0;
            for (int i = 0; i < kLedCount; i++) {
                const bool left = i < kLedCount / 2;
                if (red == left) setLedRaw(i, 255, 0, 0);
                else setLedRaw(i, 0, 0, 255);
            }
            break;
        }
        case LedEffect::Fire: {
            for (int i = 0; i < kLedCount; i++) {
                const uint8_t flick = esp_random() & 0x7F;
                const uint8_t r = static_cast<uint8_t>(std::min(255, 200 + flick / 2));
                const uint8_t g = static_cast<uint8_t>(40 + (flick / 2));
                setLedRaw(i, r, g, 0);
            }
            break;
        }
        case LedEffect::Chase: {
            const int pos = ledFrame % kLedCount;
            for (int i = 0; i < kLedCount; i++) {
                const int d = (i - pos + kLedCount) % kLedCount;
                if (d == 0) setLedRaw(i, ledR, ledG, ledB);
                else if (d == 1) setLedRaw(i, ledR / 3, ledG / 3, ledB / 3);
                else if (d == 2) setLedRaw(i, ledR / 8, ledG / 8, ledB / 8);
                else setLedRaw(i, 0, 0, 0);
            }
            break;
        }
        case LedEffect::Theater: {
            const int step = ledFrame % 3;
            for (int i = 0; i < kLedCount; i++) {
                if (i % 3 == step) setLedRaw(i, ledR, ledG, ledB);
                else setLedRaw(i, 0, 0, 0);
            }
            break;
        }
        case LedEffect::Listening: {
            // soft cyan breathing both sides
            const float t = fmodf((float)ledFrame * 0.08f, 6.28318f);
            const float amp = 0.3f + (sinf(t) + 1.0f) * 0.35f;
            for (int i = 0; i < kLedCount; i++) {
                setLedRaw(i, 0, static_cast<uint8_t>(80 * amp), static_cast<uint8_t>(200 * amp));
            }
            break;
        }
        case LedEffect::Thinking: {
            // a single dot rotates around the head
            const int pos = ledFrame % kLedCount;
            for (int i = 0; i < kLedCount; i++) {
                const int d = (i - pos + kLedCount) % kLedCount;
                if (d == 0) setLedRaw(i, 180, 80, 220);
                else setLedRaw(i, 20, 6, 30);
            }
            break;
        }
        case LedEffect::Talking: {
            // both sides pulse green
            const float t = fmodf((float)ledFrame * 0.25f, 6.28318f);
            const float amp = 0.4f + (sinf(t) + 1.0f) * 0.3f;
            for (int i = 0; i < kLedCount; i++) {
                setLedRaw(i, 0, static_cast<uint8_t>(220 * amp), static_cast<uint8_t>(60 * amp));
            }
            break;
        }
        case LedEffect::Recording: {
            // Hard on/off red blink at ~2.5 Hz — universally "recording".
            // Time-based (not ledFrame-based) so cadence is consistent.
            const bool on = (millis() / 200) & 1;
            for (int i = 0; i < kLedCount; i++) {
                if (on) setLedRaw(i, 255, 0, 0);   // bright red
                else    setLedRaw(i, 0, 0, 0);     // off
            }
            break;
        }
    }
    refreshLeds();
}

void ledLoopUpdate()
{
    if (ledEffect == LedEffect::Solid) {
        // Only refresh once after setting; no animation needed.
        return;
    }
    const uint32_t now = millis();
    if (now - lastLedFrameMs < ledSpeed) return;
    lastLedFrameMs = now;
    ledFrame++;
    ledTickEffect();
}

void setLedEffect(LedEffect effect, uint8_t r, uint8_t g, uint8_t b, uint16_t speedMs)
{
    ledEffect = effect;
    ledR = r;
    ledG = g;
    ledB = b;
    if (speedMs == 0) speedMs = 80;
    ledSpeed = speedMs;
    ledFrame = 0;
    lastLedFrameMs = 0;
    ledTickEffect();
}

// =========================================================================
// Display: status + paged message + face + big + marquee + progress
// =========================================================================

void drawStatusScreen()
{
    auto& display = M5StackChan.Display();
    display.fillScreen(TFT_BLACK);
    display.setTextScroll(false);
    display.setTextWrap(true);
    display.setTextSize(1);
    display.setTextColor(TFT_WHITE, TFT_BLACK);
    display.setFont(&fonts::efontCN_24);
    display.setCursor(12, 12);
    display.println("StackChan");
    display.setFont(&fonts::efontCN_16);
    display.setTextColor(TFT_CYAN, TFT_BLACK);
    display.println();
    display.printf("OpenClaw: %s\n", OPENCLAW_GATEWAY_URL);
    display.printf("WiFi: %s\n", WiFi.isConnected() ? WiFi.SSID().c_str() : "AP mode");
    display.printf("IP: %s\n", WiFi.localIP().toString().c_str());
    display.printf("Host: %s.local\n", kHostName);
    display.printf("Firmware: v%s\n", kFirmwareVersion);
    display.println();
    display.setTextColor(TFT_ORANGE, TFT_BLACK);
    display.println("HTTP API ready");
    displayMode = DisplayMode::Status;
}

void splitTextIntoLines(const String& text, int maxWidth, std::vector<String>& out)
{
    auto& display = M5StackChan.Display();
    out.clear();
    String current;
    const int len = text.length();
    int i = 0;
    while (i < len) {
        const char c = text[i];
        if (c == '\n') {
            out.push_back(current);
            current = "";
            i++;
            continue;
        }
        if (c == '\r') { i++; continue; }
        const int bytes = utf8CharBytes(static_cast<uint8_t>(c));
        const String chunk = text.substring(i, i + bytes);
        const String candidate = current + chunk;
        if (current.length() > 0 && display.textWidth(candidate.c_str()) > maxWidth) {
            out.push_back(current);
            current = chunk;
        } else {
            current = candidate;
        }
        i += bytes;
    }
    if (current.length() > 0) out.push_back(current);
    if (out.empty()) out.push_back("");
}

void renderMessagePage()
{
    auto& display = M5StackChan.Display();
    display.fillScreen(TFT_BLACK);
    display.setTextScroll(false);
    display.setTextWrap(false);
    display.setTextSize(1);

    display.setFont(&fonts::efontCN_24);
    display.setTextColor(TFT_CYAN, TFT_BLACK);
    display.setCursor(kSideMargin, 10);
    display.println(lastTitle);
    display.drawFastHLine(kSideMargin, 44, display.width() - 2 * kSideMargin, TFT_DARKGREY);

    display.setFont(&fonts::efontCN_16);
    display.setTextColor(TFT_WHITE, TFT_BLACK);
    const int start = currentPage * kRowsPerPage;
    const int end = std::min(static_cast<int>(messageLines.size()), start + kRowsPerPage);
    int y = kContentTop;
    for (int i = start; i < end; i++) {
        display.setCursor(kSideMargin, y);
        display.print(messageLines[i]);
        y += kLineHeight;
    }

    if (totalPages > 1) {
        char pageBuf[16];
        snprintf(pageBuf, sizeof(pageBuf), "%d/%d", currentPage + 1, totalPages);
        display.setTextColor(TFT_DARKGREY, TFT_BLACK);
        const int w = display.textWidth(pageBuf);
        display.setCursor(display.width() - w - 8, display.height() - 18);
        display.print(pageBuf);
    }
}

void prepareMessagePages()
{
    auto& display = M5StackChan.Display();
    display.setFont(&fonts::efontCN_16);
    splitTextIntoLines(lastText, display.width() - 2 * kSideMargin, messageLines);
    totalPages = (static_cast<int>(messageLines.size()) + kRowsPerPage - 1) / kRowsPerPage;
    if (totalPages < 1) totalPages = 1;
    currentPage = 0;
    lastPageMillis = millis();
}

void drawMessage()
{
    displayMode = DisplayMode::Message;
    prepareMessagePages();
    renderMessagePage();
}

// ---------------- Face renderer ----------------

struct FaceParams {
    int eyeY = 110;
    int eyeR = 28;
    int eyeDx = 60;
    int mouthY = 185;
    int mouthW = 70;
    int mouthH = 18;
    bool leftClosed = false;
    bool rightClosed = false;
    int pupilOffsetX = 0;
    int pupilOffsetY = 0;
    int browAngle = 0;   // negative tilts inner-down (angry), positive tilts up (sad)
    int mouthShape = 0;  // 0=neutral line, 1=smile arc, 2=frown arc, 3=open ellipse, 4=small dot, 5=cat
    bool showTongue = false;
    bool showZ = false;
    bool showHeart = false;
    bool showSweat = false;
};

void drawEye(int cx, int cy, const FaceParams& p, bool closed, uint16_t eyeC, uint16_t bgC)
{
    auto& d = M5StackChan.Display();
    if (closed || blinking) {
        d.drawFastHLine(cx - p.eyeR, cy, p.eyeR * 2, eyeC);
        d.drawFastHLine(cx - p.eyeR, cy + 1, p.eyeR * 2, eyeC);
        return;
    }
    d.fillCircle(cx, cy, p.eyeR, eyeC);
    d.fillCircle(cx, cy, p.eyeR - 4, bgC);
    // pupil
    d.fillCircle(cx + p.pupilOffsetX, cy + p.pupilOffsetY, p.eyeR - 12, eyeC);
    // highlight
    d.fillCircle(cx + p.pupilOffsetX - 6, cy + p.pupilOffsetY - 6, 3, to565(0xFFFFFF));
}

void drawMouth(const FaceParams& p, uint16_t mouthC, uint16_t bgC)
{
    auto& d = M5StackChan.Display();
    const int cx = d.width() / 2;
    switch (p.mouthShape) {
        case 0: // neutral line
            d.fillRoundRect(cx - p.mouthW / 2, p.mouthY - 2, p.mouthW, 5, 2, mouthC);
            break;
        case 1: { // smile arc
            for (int t = 0; t <= 12; t++) {
                const float a = (float)t / 12.0f;
                const int x = cx - p.mouthW / 2 + (int)(a * p.mouthW);
                const int y = p.mouthY + (int)(sinf(a * 3.14159f) * p.mouthH);
                d.fillCircle(x, y, 3, mouthC);
            }
            break;
        }
        case 2: { // frown
            for (int t = 0; t <= 12; t++) {
                const float a = (float)t / 12.0f;
                const int x = cx - p.mouthW / 2 + (int)(a * p.mouthW);
                const int y = p.mouthY - (int)(sinf(a * 3.14159f) * p.mouthH);
                d.fillCircle(x, y, 3, mouthC);
            }
            break;
        }
        case 3: { // open (surprised)
            d.fillCircle(cx, p.mouthY, p.mouthH, mouthC);
            d.fillCircle(cx, p.mouthY, p.mouthH - 4, bgC);
            break;
        }
        case 4: { // small dot
            d.fillCircle(cx, p.mouthY, 5, mouthC);
            break;
        }
        case 5: { // cat 3
            d.drawLine(cx - 16, p.mouthY - 6, cx, p.mouthY + 6, mouthC);
            d.drawLine(cx, p.mouthY + 6, cx + 16, p.mouthY - 6, mouthC);
            d.drawLine(cx - 16, p.mouthY - 5, cx, p.mouthY + 7, mouthC);
            d.drawLine(cx, p.mouthY + 7, cx + 16, p.mouthY - 5, mouthC);
            break;
        }
    }
    if (p.showTongue) {
        d.fillRoundRect(cx - 10, p.mouthY + 8, 20, 14, 5, to565(0xFF5577));
    }
}

void drawBrows(const FaceParams& p, uint16_t c)
{
    auto& d = M5StackChan.Display();
    const int cx = d.width() / 2;
    const int leftEye = cx - p.eyeDx;
    const int rightEye = cx + p.eyeDx;
    const int browY = p.eyeY - p.eyeR - 10;
    if (p.browAngle < 0) {
        // angry: inner ends drop
        d.fillTriangle(leftEye - 16, browY, leftEye + 16, browY + 12, leftEye + 16, browY - 2, c);
        d.fillTriangle(rightEye + 16, browY, rightEye - 16, browY + 12, rightEye - 16, browY - 2, c);
    } else if (p.browAngle > 0) {
        // sad: inner ends rise
        d.fillTriangle(leftEye - 16, browY + 12, leftEye + 16, browY, leftEye - 16, browY + 14, c);
        d.fillTriangle(rightEye + 16, browY + 12, rightEye - 16, browY, rightEye + 16, browY + 14, c);
    } else {
        d.fillRoundRect(leftEye - 16, browY, 32, 4, 2, c);
        d.fillRoundRect(rightEye - 16, browY, 32, 4, 2, c);
    }
}

void drawDecorations(const FaceParams& p, uint16_t accentC)
{
    auto& d = M5StackChan.Display();
    const int cx = d.width() / 2;
    if (p.showZ) {
        d.setFont(&fonts::efontCN_24);
        d.setTextColor(to565(0xCCCCFF), to565(faceBgColor));
        d.setCursor(d.width() - 60, 30);
        d.print("z");
        d.setCursor(d.width() - 40, 50);
        d.print("Z");
    }
    if (p.showHeart) {
        const int hx = 50;
        const int hy = 50;
        d.fillCircle(hx - 6, hy, 8, to565(0xFF5577));
        d.fillCircle(hx + 6, hy, 8, to565(0xFF5577));
        d.fillTriangle(hx - 13, hy + 3, hx + 13, hy + 3, hx, hy + 18, to565(0xFF5577));
    }
    if (p.showSweat) {
        d.fillCircle(cx + 70, p.eyeY - 20, 5, to565(0x66BBEE));
        d.fillTriangle(cx + 65, p.eyeY - 20, cx + 75, p.eyeY - 20, cx + 70, p.eyeY - 32, to565(0x66BBEE));
    }
    (void)accentC;
}

void presetExpression(const String& name, FaceParams& p)
{
    p = FaceParams();
    if (name == "neutral") {
        p.mouthShape = 0;
    } else if (name == "happy") {
        p.mouthShape = 1;
    } else if (name == "smile") {
        p.mouthShape = 1;
        p.eyeR = 24;
    } else if (name == "love") {
        p.mouthShape = 1;
        p.showHeart = true;
        p.eyeR = 22;
    } else if (name == "sad") {
        p.mouthShape = 2;
        p.browAngle = 1;
    } else if (name == "angry") {
        p.mouthShape = 2;
        p.browAngle = -1;
        p.eyeR = 24;
    } else if (name == "surprised") {
        p.mouthShape = 3;
        p.eyeR = 32;
    } else if (name == "thinking") {
        p.mouthShape = 4;
        p.pupilOffsetX = 12;
        p.pupilOffsetY = -8;
        p.browAngle = -1;
    } else if (name == "sleep") {
        p.leftClosed = true;
        p.rightClosed = true;
        p.mouthShape = 4;
        p.showZ = true;
    } else if (name == "wink_l") {
        p.leftClosed = true;
        p.mouthShape = 1;
    } else if (name == "wink_r") {
        p.rightClosed = true;
        p.mouthShape = 1;
    } else if (name == "stare") {
        p.mouthShape = 0;
        p.eyeR = 30;
    } else if (name == "dead") {
        p.leftClosed = false;
        p.rightClosed = false;
        p.mouthShape = 0;
        p.pupilOffsetX = -8;
    } else if (name == "embarrassed") {
        p.mouthShape = 1;
        p.showSweat = true;
        p.browAngle = 1;
    } else if (name == "cat") {
        p.mouthShape = 5;
        p.eyeR = 22;
    } else if (name == "speak" || name == "talking") {
        p.mouthShape = 3;
        p.mouthH = 10;
    } else {
        p.mouthShape = 0;
    }
}

void renderFace()
{
    auto& d = M5StackChan.Display();
    const uint16_t bg = to565(faceBgColor);
    const uint16_t eyeC = to565(faceEyeColor);
    const uint16_t mouthC = to565(faceMouthColor);
    d.fillScreen(bg);

    FaceParams p;
    presetExpression(faceExpression, p);

    drawBrows(p, eyeC);

    const int cx = d.width() / 2;
    drawEye(cx - p.eyeDx, p.eyeY, p, p.leftClosed, eyeC, bg);
    drawEye(cx + p.eyeDx, p.eyeY, p, p.rightClosed, eyeC, bg);
    drawMouth(p, mouthC, bg);
    drawDecorations(p, eyeC);
    lastFaceFrameMs = millis();
}

void drawFace(const String& expression, uint32_t eye, uint32_t mouth, uint32_t bg)
{
    faceExpression = expression;
    faceEyeColor = eye;
    faceMouthColor = mouth;
    faceBgColor = bg;
    displayMode = DisplayMode::Face;
    blinking = false;
    blinkUntilMs = 0;
    lastBlinkMs = millis();
    renderFace();
}

void displayBig(const String& text, uint32_t color)
{
    auto& d = M5StackChan.Display();
    bigText = text;
    bigColor = color;
    d.fillScreen(TFT_BLACK);
    d.setFont(&fonts::efontCN_24);
    d.setTextColor(to565(color), TFT_BLACK);
    d.setTextSize(2);
    const int tw = d.textWidth(text.c_str());
    const int th = 48;
    d.setCursor(std::max<int>(0, (d.width() - tw) / 2), (d.height() - th) / 2);
    d.print(text);
    d.setTextSize(1);
    displayMode = DisplayMode::Big;
}

void displayProgress()
{
    auto& d = M5StackChan.Display();
    d.fillScreen(TFT_BLACK);
    d.setFont(&fonts::efontCN_24);
    d.setTextColor(TFT_WHITE, TFT_BLACK);
    d.setCursor(kSideMargin, 30);
    d.print(progressLabel);
    const int x = kSideMargin;
    const int y = 120;
    const int w = d.width() - 2 * kSideMargin;
    const int h = 28;
    d.drawRoundRect(x, y, w, h, 8, TFT_WHITE);
    const int fillW = (w - 4) * progressPercent / 100;
    d.fillRoundRect(x + 2, y + 2, fillW, h - 4, 6, TFT_CYAN);
    d.setFont(&fonts::efontCN_24);
    char pct[8];
    snprintf(pct, sizeof(pct), "%d%%", progressPercent);
    d.setCursor((d.width() - d.textWidth(pct)) / 2, y + h + 10);
    d.print(pct);
    displayMode = DisplayMode::Progress;
}

void renderMarqueeFrame()
{
    auto& d = M5StackChan.Display();
    d.setFont(&fonts::efontCN_24);
    const int tw = d.textWidth(marqueeText.c_str());
    if (tw == 0) return;
    d.fillScreen(TFT_BLACK);
    d.setTextColor(to565(marqueeColor), TFT_BLACK);
    const int y = (d.height() - 30) / 2;
    int x = d.width() - marqueePixelOffset;
    while (x < d.width()) {
        d.setCursor(x, y);
        d.print(marqueeText);
        x += tw + 40;
    }
    marqueePixelOffset += 4;
    if (marqueePixelOffset > tw + 40 + d.width()) marqueePixelOffset = 0;
}

void displayClear()
{
    auto& d = M5StackChan.Display();
    d.fillScreen(TFT_BLACK);
    displayMode = DisplayMode::Clear;
}

// =========================================================================
// Motion
// =========================================================================

void moveHead(int x, int y, int speed)
{
    currentX = clampInt(x, kHeadXMin, kHeadXMax);
    currentY = clampInt(y, kHeadYMin, kHeadYMax);
    speed = clampInt(speed, kSpeedMin, kSpeedMax);
    M5StackChan.Motion.move(currentX, currentY, speed);
}

void actionHome()
{
    M5StackChan.Motion.goHome();
    currentX = 0;
    currentY = 0;
}

void actionNod()
{
    moveHead(currentX, 450, 600);
    delay(220);
    moveHead(currentX, 100, 600);
    delay(220);
    moveHead(currentX, 250, 600);
}

void actionShake()
{
    const int prevY = currentY;
    moveHead(-450, prevY, 700);
    delay(220);
    moveHead(450, prevY, 700);
    delay(220);
    moveHead(0, prevY, 700);
}

void actionYes()
{
    actionNod();
    delay(160);
    actionNod();
}

void actionNo()
{
    actionShake();
    delay(160);
    actionShake();
}

void actionLookAround()
{
    moveHead(-700, 350, 500);
    delay(400);
    moveHead(700, 350, 500);
    delay(400);
    moveHead(0, 250, 500);
}

void actionDance()
{
    for (int i = 0; i < 4; i++) {
        moveHead(-400, 600, 800);
        delay(160);
        moveHead(400, 200, 800);
        delay(160);
    }
    moveHead(0, 300, 500);
}

void actionSurprised()
{
    moveHead(currentX, 750, 1000);
    delay(120);
    moveHead(currentX, 400, 500);
}

void actionSleep()
{
    moveHead(0, 80, 200);
}

void actionWake()
{
    moveHead(0, 600, 700);
    delay(180);
    moveHead(0, 300, 400);
}

void actionPanic()
{
    for (int i = 0; i < 5; i++) {
        moveHead((esp_random() % 1601) - 800, 100 + (esp_random() % 600), 1000);
        delay(80);
    }
    moveHead(0, 300, 500);
}

void actionPeek()
{
    moveHead(-300, 700, 400);
    delay(220);
    moveHead(-300, 200, 400);
    delay(180);
    moveHead(0, 250, 400);
}

void actionTilt(bool left)
{
    moveHead(left ? -550 : 550, 350, 400);
}

void actionBow()
{
    moveHead(0, 850, 700);
    delay(420);
    moveHead(0, 250, 400);
}

void actionTrack(float nx, float ny, int speed)
{
    nx = clampFloat(nx, -1.0f, 1.0f);
    ny = clampFloat(ny, -1.0f, 1.0f);
    const int x = static_cast<int>(nx * kHeadXMax);
    const int y = static_cast<int>(((ny + 1.0f) * 0.5f) * kHeadYMax);
    moveHead(x, y, speed);
}

// =========================================================================
// Speak task (existing, slightly trimmed for readability)
// =========================================================================

void speakSetState(bool playing, uint32_t id, const String& url, const String& title)
{
    if (speakStateMutex && xSemaphoreTake(speakStateMutex, portMAX_DELAY) == pdTRUE) {
        speakPlaying = playing;
        speakPlayingId = id;
        speakPlayingUrl = url;
        speakPlayingTitle = title;
        xSemaphoreGive(speakStateMutex);
    }
}

void speakGetState(bool& playing, uint32_t& id, String& url, String& title)
{
    if (speakStateMutex && xSemaphoreTake(speakStateMutex, portMAX_DELAY) == pdTRUE) {
        playing = speakPlaying;
        id = speakPlayingId;
        url = speakPlayingUrl;
        title = speakPlayingTitle;
        xSemaphoreGive(speakStateMutex);
    }
}

bool parseWavHeader(Client& stream, uint32_t timeoutMs, uint32_t& sampleRateOut, bool& stereoOut)
{
    uint8_t buf[12];
    size_t got = 0;
    const uint32_t deadline = millis() + timeoutMs;
    while (got < sizeof(buf) && millis() < deadline && !speakStopRequested) {
        if (stream.available() <= 0) { vTaskDelay(pdMS_TO_TICKS(5)); continue; }
        int n = stream.read(buf + got, sizeof(buf) - got);
        if (n > 0) got += n;
    }
    if (got < sizeof(buf) || std::memcmp(buf, "RIFF", 4) != 0 || std::memcmp(buf + 8, "WAVE", 4) != 0) return false;
    while (!speakStopRequested && millis() < deadline) {
        uint8_t chunkHdr[8];
        size_t hdrGot = 0;
        while (hdrGot < sizeof(chunkHdr) && millis() < deadline && !speakStopRequested) {
            if (stream.available() <= 0) { vTaskDelay(pdMS_TO_TICKS(5)); continue; }
            int n = stream.read(chunkHdr + hdrGot, sizeof(chunkHdr) - hdrGot);
            if (n > 0) hdrGot += n;
        }
        if (hdrGot < sizeof(chunkHdr)) return false;
        const uint32_t chunkSize = readUint32LE(chunkHdr + 4);
        if (std::memcmp(chunkHdr, "fmt ", 4) == 0) {
            std::vector<uint8_t> fmt(chunkSize);
            size_t fmtGot = 0;
            while (fmtGot < chunkSize && millis() < deadline && !speakStopRequested) {
                if (stream.available() <= 0) { vTaskDelay(pdMS_TO_TICKS(5)); continue; }
                int n = stream.read(fmt.data() + fmtGot, chunkSize - fmtGot);
                if (n > 0) fmtGot += n;
            }
            if (fmtGot < chunkSize || fmtGot < 16) return false;
            const uint16_t format = readUint16LE(fmt.data());
            const uint16_t channels = readUint16LE(fmt.data() + 2);
            const uint32_t sampleRate = readUint32LE(fmt.data() + 4);
            const uint16_t bitsPerSample = readUint16LE(fmt.data() + 14);
            if (format != 1 || bitsPerSample != 16 || (channels != 1 && channels != 2)) {
                Serial.printf("[speak] unsupported wav: fmt=%u ch=%u bits=%u\n", format, channels, bitsPerSample);
                return false;
            }
            sampleRateOut = sampleRate;
            stereoOut = (channels == 2);
        } else if (std::memcmp(chunkHdr, "data", 4) == 0) {
            return true;
        } else {
            size_t skipped = 0;
            while (skipped < chunkSize && millis() < deadline && !speakStopRequested) {
                uint8_t skipBuf[64];
                size_t want = std::min<size_t>(sizeof(skipBuf), chunkSize - skipped);
                if (stream.available() <= 0) { vTaskDelay(pdMS_TO_TICKS(5)); continue; }
                int n = stream.read(skipBuf, want);
                if (n > 0) skipped += n;
            }
            if (skipped < chunkSize) return false;
        }
    }
    return false;
}

void speakTask(void* /*arg*/)
{
    SpeakRequest req;
    while (true) {
        if (xQueueReceive(speakQueue, &req, portMAX_DELAY) != pdTRUE) continue;
        speakStopRequested = false;
        speakSetState(true, req.id, String(req.url), req.hasTitle ? String(req.title) : String(""));
        Serial.printf("[speak] id=%u url=%s\n", req.id, req.url);
        pushEvent("speak.start", req.hasTitle ? req.title : req.url);

        if (req.hasTitle) {
            lastTitle = req.title;
            lastText = "(speaking...)";
            drawMessage();
        }

        HTTPClient http;
        http.setTimeout(kSpeakHttpTimeoutMs);
        http.setConnectTimeout(kSpeakHttpTimeoutMs);
        if (!http.begin(req.url)) {
            Serial.println("[speak] http.begin failed");
            pushEvent("speak.error", "http_begin");
            speakSetState(false, 0, "", "");
            continue;
        }
        const int code = http.GET();
        if (code != 200) {
            Serial.printf("[speak] HTTP %d\n", code);
            char body[40];
            snprintf(body, sizeof(body), "http_%d", code);
            pushEvent("speak.error", body);
            http.end();
            speakSetState(false, 0, "", "");
            continue;
        }

        WiFiClient* stream = http.getStreamPtr();
        uint32_t sampleRate = kSpeakDefaultSampleRate;
        bool stereo = false;
        if (!parseWavHeader(*stream, kSpeakHttpTimeoutMs, sampleRate, stereo)) {
            Serial.println("[speak] bad WAV header");
            pushEvent("speak.error", "bad_wav");
            http.end();
            speakSetState(false, 0, "", "");
            continue;
        }
        Serial.printf("[speak] PCM %uHz %s\n", sampleRate, stereo ? "stereo" : "mono");

        const uint32_t startMs = millis();
        const int contentLen = http.getSize();
        size_t bufCap = (contentLen > 0 && static_cast<size_t>(contentLen) <= kSpeakMaxBytes)
                           ? static_cast<size_t>(contentLen)
                           : 512 * 1024;
        uint8_t* audio = static_cast<uint8_t*>(heap_caps_malloc(bufCap, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        if (audio == nullptr) audio = static_cast<uint8_t*>(heap_caps_malloc(bufCap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (audio == nullptr) {
            Serial.printf("[speak] OOM (wanted %u bytes)\n", (uint32_t)bufCap);
            pushEvent("speak.error", "oom");
            http.end();
            speakSetState(false, 0, "", "");
            continue;
        }

        size_t audioBytes = 0;
        const uint32_t downloadDeadline = millis() + 30000;
        uint32_t idleSince = 0;
        while (!speakStopRequested && audioBytes < bufCap && millis() < downloadDeadline) {
            size_t want = std::min<size_t>(bufCap - audioBytes, kSpeakChunkBytes);
            want &= ~static_cast<size_t>(1);
            if (want == 0) break;
            int n = stream->read(audio + audioBytes, want);
            if (n > 0) { audioBytes += n; idleSince = 0; continue; }
            if (!http.connected()) {
                if (idleSince == 0) idleSince = millis();
                else if (millis() - idleSince > 200) break;
            }
            vTaskDelay(pdMS_TO_TICKS(1));
        }

        Serial.printf("[speak] downloaded %u bytes in %u ms\n", (uint32_t)audioBytes, millis() - startMs);

        if (!speakStopRequested && audioBytes >= 2) {
            const int16_t* samples = reinterpret_cast<const int16_t*>(audio);
            const size_t sampleCount = audioBytes / 2;
            if (!M5.Speaker.playRaw(samples, sampleCount, sampleRate, stereo)) {
                Serial.println("[speak] playRaw rejected");
                pushEvent("speak.error", "play_raw");
            }
            while (!speakStopRequested && M5.Speaker.isPlaying()) {
                vTaskDelay(pdMS_TO_TICKS(20));
            }
        }

        if (speakStopRequested) {
            M5.Speaker.stop();
            while (M5.Speaker.isPlaying()) vTaskDelay(pdMS_TO_TICKS(10));
            Serial.printf("[speak] stopped (after %ums)\n", millis() - startMs);
            pushEvent("speak.stopped", "");
        } else {
            Serial.printf("[speak] done (after %ums)\n", millis() - startMs);
            pushEvent("speak.done", "");
        }

        free(audio);
        http.end();
        speakSetState(false, 0, "", "");
    }
}

void speakStartTask()
{
    speakQueue = xQueueCreate(kSpeakQueueLen, sizeof(SpeakRequest));
    speakStateMutex = xSemaphoreCreateMutex();
    xTaskCreatePinnedToCore(speakTask, "speakTask", 8192, nullptr, 1, &speakTaskHandle, 1);
}

// =========================================================================
// Mic task (record + optional HTTP upload)
// =========================================================================

void micSetState(bool recording, uint32_t id)
{
    if (micStateMutex && xSemaphoreTake(micStateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        micRecording = recording;
        micRecordingId = id;
        xSemaphoreGive(micStateMutex);
    }
}

void micStoreLast(uint8_t* wav, size_t len, const String& note)
{
    if (micBufMutex == nullptr) return;
    if (xSemaphoreTake(micBufMutex, pdMS_TO_TICKS(30)) != pdTRUE) return;
    if (micLastWav) { free(micLastWav); micLastWav = nullptr; micLastWavLen = 0; }
    micLastWav = wav;
    micLastWavLen = len;
    micLastNote = note;
    xSemaphoreGive(micBufMutex);
}

void buildWavHeader(uint8_t* hdr, uint32_t sampleRate, uint32_t pcmBytes)
{
    std::memcpy(hdr, "RIFF", 4);
    writeUint32LE(hdr + 4, 36 + pcmBytes);
    std::memcpy(hdr + 8, "WAVE", 4);
    std::memcpy(hdr + 12, "fmt ", 4);
    writeUint32LE(hdr + 16, 16);
    writeUint16LE(hdr + 20, 1);    // PCM
    writeUint16LE(hdr + 22, 1);    // mono
    writeUint32LE(hdr + 24, sampleRate);
    writeUint32LE(hdr + 28, sampleRate * 2);  // byte rate
    writeUint16LE(hdr + 32, 2);    // block align
    writeUint16LE(hdr + 34, 16);   // bits
    std::memcpy(hdr + 36, "data", 4);
    writeUint32LE(hdr + 40, pcmBytes);
}

void micTask(void* /*arg*/)
{
    MicRequest req;
    while (true) {
        if (xQueueReceive(micQueue, &req, portMAX_DELAY) != pdTRUE) continue;
        micStopRequested = false;
        micSetState(true, req.id);
        pushEvent("mic.start", "");

        const uint32_t sr = req.sample_rate == 0 ? kMicDefaultSampleRate : req.sample_rate;
        const uint32_t secs = clampInt(req.seconds, 1, kMicMaxSeconds);
        const size_t sampleCount = static_cast<size_t>(sr) * secs;
        const size_t pcmBytes = sampleCount * 2;
        const size_t wavLen = pcmBytes + 44;

        uint8_t* wav = static_cast<uint8_t*>(heap_caps_malloc(wavLen, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (wav == nullptr) wav = static_cast<uint8_t*>(heap_caps_malloc(wavLen, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        if (wav == nullptr) {
            Serial.printf("[mic] OOM (wanted %u bytes)\n", (uint32_t)wavLen);
            pushEvent("mic.error", "oom");
            micSetState(false, 0);
            continue;
        }

        buildWavHeader(wav, sr, pcmBytes);
        int16_t* pcm = reinterpret_cast<int16_t*>(wav + 44);

        // Stop speaker first to avoid I2S contention
        if (M5.Speaker.isPlaying()) {
            M5.Speaker.stop();
            vTaskDelay(pdMS_TO_TICKS(40));
        }

        if (!M5.Mic.isEnabled()) {
            Serial.println("[mic] mic not enabled");
            pushEvent("mic.error", "disabled");
            free(wav);
            micSetState(false, 0);
            continue;
        }
        if (!M5.Mic.begin()) {
            Serial.println("[mic] begin failed");
            pushEvent("mic.error", "begin");
            free(wav);
            micSetState(false, 0);
            continue;
        }

        const uint32_t startMs = millis();
        if (!M5.Mic.record(pcm, sampleCount, sr)) {
            Serial.println("[mic] record() rejected");
            M5.Mic.end();
            pushEvent("mic.error", "record_reject");
            free(wav);
            micSetState(false, 0);
            continue;
        }
        // Wait for recording to drain
        while (M5.Mic.isRecording() && !micStopRequested && millis() - startMs < secs * 1000 + 2000) {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
        M5.Mic.end();
        const uint32_t durMs = millis() - startMs;
        Serial.printf("[mic] captured %u samples in %u ms\n", (uint32_t)sampleCount, durMs);

        // Optional upload
        bool uploadOk = false;
        int uploadStatus = 0;
        String responseBody;
        if (req.upload && req.upload_url[0] != '\0') {
            HTTPClient http;
            http.setTimeout(20000);
            if (http.begin(req.upload_url)) {
                http.addHeader("Content-Type", "audio/wav");
                if (req.prompt[0] != '\0') http.addHeader("X-StackChan-Prompt", req.prompt);
                uploadStatus = http.POST(wav, wavLen);
                responseBody = http.getString();
                uploadOk = (uploadStatus >= 200 && uploadStatus < 300);
                Serial.printf("[mic] uploaded -> %d (%u bytes resp)\n", uploadStatus, (uint32_t)responseBody.length());
                http.end();
            } else {
                Serial.println("[mic] upload http.begin failed");
            }
        }

        // Store the WAV for /mic/last
        micStoreLast(wav, wavLen, req.upload ? (uploadOk ? "uploaded" : "upload_failed") : "captured");

        char evdata[120];
        snprintf(evdata, sizeof(evdata), "bytes=%u upload=%s status=%d", (uint32_t)wavLen,
                 req.upload ? "1" : "0", uploadStatus);
        pushEvent("mic.done", evdata);

        // If upload returned a JSON with a "speak_url" field, auto-play it
        if (uploadOk && responseBody.length() > 0) {
            StaticJsonDocument<512> resp;
            DeserializationError de = deserializeJson(resp, responseBody);
            if (!de) {
                const char* speakUrl = resp["speak_url"] | "";
                if (speakUrl && speakUrl[0] != '\0') {
                    SpeakRequest sreq = {};
                    strlcpy(sreq.url, speakUrl, sizeof(sreq.url));
                    const char* title = resp["title"] | "";
                    if (title[0] != '\0') {
                        strlcpy(sreq.title, title, sizeof(sreq.title));
                        sreq.hasTitle = true;
                    }
                    sreq.id = speakNextId++;
                    speakStopRequested = true;
                    M5.Speaker.stop();
                    SpeakRequest drain;
                    while (xQueueReceive(speakQueue, &drain, 0) == pdTRUE) {}
                    xQueueSend(speakQueue, &sreq, pdMS_TO_TICKS(200));
                }
            }
        }

        micSetState(false, 0);
    }
}

void micStartTask()
{
    micQueue = xQueueCreate(1, sizeof(MicRequest));
    micStateMutex = xSemaphoreCreateMutex();
    micBufMutex = xSemaphoreCreateMutex();
    xTaskCreatePinnedToCore(micTask, "micTask", 8192, nullptr, 1, &micTaskHandle, 0);
}

// =========================================================================
// Webhook task
// =========================================================================

void webhookTask(void* /*arg*/)
{
    WebhookMsg msg;
    while (true) {
        if (xQueueReceive(webhookQueue, &msg, portMAX_DELAY) != pdTRUE) continue;
        HTTPClient http;
        http.setTimeout(4000);
        http.setConnectTimeout(2000);
        if (!http.begin(msg.url)) continue;
        http.addHeader("Content-Type", "application/json");
        const int code = http.POST(reinterpret_cast<uint8_t*>(msg.body), strlen(msg.body));
        (void)code;
        http.end();
    }
}

void webhookStartTask()
{
    webhookQueue = xQueueCreate(8, sizeof(WebhookMsg));
    webhookCfgMutex = xSemaphoreCreateMutex();
    xTaskCreatePinnedToCore(webhookTask, "webhookTask", 4096, nullptr, 1, &webhookTaskHandle, 0);
}

// =========================================================================
// Camera (GC0308 via esp_camera)
// =========================================================================

bool cameraInit()
{
    if (cameraEnabled) return true;
    if (cameraInitTried && cameraInitError.length() > 0) {
        // Already failed; don't keep retrying every request.
        return false;
    }
    cameraInitTried = true;

    camera_config_t cfg = {};
    cfg.pin_pwdn  = kCamPinPwdn;
    cfg.pin_reset = kCamPinReset;
    cfg.pin_xclk  = kCamPinXclk;
    cfg.pin_sccb_sda = -1;     // -1 → use existing bus on `sccb_i2c_port`
    cfg.pin_sccb_scl = -1;
    cfg.pin_d7 = kCamPinD7;
    cfg.pin_d6 = kCamPinD6;
    cfg.pin_d5 = kCamPinD5;
    cfg.pin_d4 = kCamPinD4;
    cfg.pin_d3 = kCamPinD3;
    cfg.pin_d2 = kCamPinD2;
    cfg.pin_d1 = kCamPinD1;
    cfg.pin_d0 = kCamPinD0;
    cfg.pin_vsync = kCamPinVsync;
    cfg.pin_href  = kCamPinHref;
    cfg.pin_pclk  = kCamPinPclk;
    cfg.xclk_freq_hz = 20'000'000;
    cfg.ledc_timer   = LEDC_TIMER_0;
    cfg.ledc_channel = LEDC_CHANNEL_0;
    // GC0308 has no hardware JPEG encoder. Capture RGB565, software-encode to
    // JPEG in the /camera/capture handler via frame2jpg().
    cfg.pixel_format = PIXFORMAT_RGB565;
    cfg.frame_size   = cameraFrameSize;
    cfg.jpeg_quality = cameraQuality;
    cfg.fb_count     = 2;     // double buffer makes back-to-back captures fast
    cfg.fb_location  = CAMERA_FB_IN_PSRAM;
    cfg.grab_mode    = CAMERA_GRAB_WHEN_EMPTY;  // idle unless we ask for a frame
    // M5.In_I2C is on i2c port 0 (Wire) on CoreS3. The camera will reuse this
    // bus to talk to the GC0308 SCCB at 0x21 without re-initialising the pins.
    cfg.sccb_i2c_port = static_cast<int>(M5.In_I2C.getPort());

    esp_err_t err = esp_camera_init(&cfg);
    if (err != ESP_OK) {
        char buf[48];
        snprintf(buf, sizeof(buf), "esp_camera_init=0x%x", err);
        cameraInitError = buf;
        Serial.printf("[camera] init failed: %s\n", buf);
        return false;
    }
    sensor_t* s = esp_camera_sensor_get();
    if (s) {
        // GC0308 default tweaks: small adjustments for indoor lighting.
        s->set_brightness(s, 0);
        s->set_contrast(s, 0);
        s->set_saturation(s, 0);
        s->set_special_effect(s, 0);
        s->set_hmirror(s, 0);
        s->set_vflip(s, 0);
    }
    cameraEnabled = true;
    cameraInitError = "";
    Serial.println("[camera] initialised");
    pushEvent("camera.ready", "");
    return true;
}

void cameraDeinit()
{
    if (!cameraEnabled) return;
    esp_camera_deinit();
    cameraEnabled = false;
    cameraInitTried = false;
    Serial.println("[camera] deinit");
}

// Dedicated task: the ONLY context that calls esp_camera_*. Waits for a job
// notification, then init -> capture -> software-encode JPEG -> deinit, all here.
//
// Why deinit after every shot: the esp32-camera driver streams continuously once
// initialised, and on this Arduino/ESP32-S3 stack that continuous DMA activity
// permanently destabilises WiFi — the device becomes unreachable and does not
// recover (verified 2026-05-15: capture+encode succeed on serial in ~68ms, but
// the network dies and stays dead while the camera stays initialised). The
// official m5stack firmware keeps the camera alive, but that's a different stack
// (ESP-IDF + esp_video V4L2 driver). For our periodic-snapshot use (tracking
// captures every few seconds) keeping the camera OFF between shots is correct:
// the ~250ms re-init cost is irrelevant and the network stays up between shots.
// The old crash was deinit running on the WebServer task; here it's serialized
// on cameraTask and the web task never touches the camera at all.
void cameraTask(void* /*arg*/)
{
    while (true) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        cameraJob.jpg = nullptr;
        cameraJob.jpgLen = 0;
        cameraJob.captureMs = 0;
        cameraJob.ok = false;
        cameraJob.err[0] = '\0';

        const uint32_t t0 = millis();
        if (!cameraInit()) {
            snprintf(cameraJob.err, sizeof(cameraJob.err), "%s",
                     cameraInitError.length() > 0 ? cameraInitError.c_str() : "camera init failed");
            xSemaphoreGive(cameraJobDone);
            continue;
        }

        camera_fb_t* fb = esp_camera_fb_get();
        uint8_t* jpg = nullptr;
        size_t jpgLen = 0;
        if (fb) {
            const uint8_t jpgQ = static_cast<uint8_t>(clampInt(80 - cameraQuality, 5, 95));
            if (!frame2jpg(fb, jpgQ, &jpg, &jpgLen)) {
                if (jpg) { free(jpg); jpg = nullptr; }
                jpgLen = 0;
            }
            esp_camera_fb_return(fb);
        }

        cameraDeinit();   // camera OFF again — WiFi is unimpeded between shots

        if (!fb) {
            snprintf(cameraJob.err, sizeof(cameraJob.err), "fb_get failed");
        } else if (jpg == nullptr) {
            snprintf(cameraJob.err, sizeof(cameraJob.err), "jpeg encode failed");
        } else {
            cameraJob.jpg = jpg;
            cameraJob.jpgLen = jpgLen;
            cameraJob.captureMs = millis() - t0;
            cameraJob.ok = true;
            cameraLastCaptureMs = cameraJob.captureMs;
            cameraLastBytes = jpgLen;
        }
        xSemaphoreGive(cameraJobDone);
    }
}

// Run one capture + encode on cameraTask, blocking the caller until it finishes.
// On success *outJpg points at a heap buffer the CALLER must free(); on failure
// errBuf is filled. Safe to call from the WebServer task or the WS task.
bool cameraRunJob(uint8_t** outJpg, size_t* outLen, uint32_t* outMs,
                  char* errBuf, size_t errBufLen, uint32_t timeoutMs)
{
    *outJpg = nullptr;
    *outLen = 0;
    if (outMs) *outMs = 0;
    if (cameraTaskHandle == nullptr || cameraJobDone == nullptr) {
        snprintf(errBuf, errBufLen, "camera task not running");
        return false;
    }
    if (xSemaphoreTake(cameraMutex, pdMS_TO_TICKS(timeoutMs)) != pdTRUE) {
        snprintf(errBuf, errBufLen, "camera busy");
        return false;
    }
    xSemaphoreTake(cameraJobDone, 0);   // drain any stale completion signal
    xTaskNotifyGive(cameraTaskHandle);
    if (xSemaphoreTake(cameraJobDone, pdMS_TO_TICKS(timeoutMs)) != pdTRUE) {
        // Job overran the timeout. Deliberately keep cameraMutex HELD: a frame may
        // still be in flight and would clobber cameraJob. This freezes only the
        // camera path — the rest of the device stays alive (the whole point of
        // moving camera work off the WebServer task). Recovers on reboot.
        Serial.println("[camera] job timeout — camera path frozen until reboot");
        snprintf(errBuf, errBufLen, "camera timeout");
        return false;
    }
    const bool ok = cameraJob.ok;
    if (ok) {
        *outJpg = cameraJob.jpg;
        *outLen = cameraJob.jpgLen;
        if (outMs) *outMs = cameraJob.captureMs;
    } else {
        snprintf(errBuf, errBufLen, "%s", cameraJob.err);
    }
    xSemaphoreGive(cameraMutex);
    return ok;
}

framesize_t parseFrameSize(const String& s, framesize_t fallback)
{
    if (s == "96x96")    return FRAMESIZE_96X96;
    if (s == "qqvga")    return FRAMESIZE_QQVGA;   // 160x120
    if (s == "qcif")     return FRAMESIZE_QCIF;    // 176x144
    if (s == "hqvga")    return FRAMESIZE_HQVGA;   // 240x176
    if (s == "240x240")  return FRAMESIZE_240X240;
    if (s == "qvga")     return FRAMESIZE_QVGA;    // 320x240
    if (s == "cif")      return FRAMESIZE_CIF;     // 400x296
    if (s == "hvga")     return FRAMESIZE_HVGA;    // 480x320
    if (s == "vga")      return FRAMESIZE_VGA;     // 640x480
    return fallback;
}

const char* frameSizeName(framesize_t f)
{
    switch (f) {
        case FRAMESIZE_96X96: return "96x96";
        case FRAMESIZE_QQVGA: return "qqvga";
        case FRAMESIZE_QCIF: return "qcif";
        case FRAMESIZE_HQVGA: return "hqvga";
        case FRAMESIZE_240X240: return "240x240";
        case FRAMESIZE_QVGA: return "qvga";
        case FRAMESIZE_CIF: return "cif";
        case FRAMESIZE_HVGA: return "hvga";
        case FRAMESIZE_VGA: return "vga";
        default: return "other";
    }
}

// =========================================================================
// Sensor poll (IMU, head touch, screen touch, buttons, battery)
// =========================================================================

void sensorPoll()
{
    const uint32_t now = millis();
    if (now - lastSensorPollMs < kSensorPollIntervalMs) return;
    lastSensorPollMs = now;

    auto& imu = M5.Imu;
    const auto mask = imu.update();
    if (mask) {
        auto d = imu.getImuData();
        lastAccelX = d.accel.x;
        lastAccelY = d.accel.y;
        lastAccelZ = d.accel.z;
        lastGyroX = d.gyro.x;
        lastGyroY = d.gyro.y;
        lastGyroZ = d.gyro.z;
        if (mask & m5::IMU_Class::sensor_mask_mag) {
            lastMagX = d.mag.x;
            lastMagY = d.mag.y;
            lastMagZ = d.mag.z;
            hasMagnetometer = true;
        }
        const float mag = sqrtf(d.accel.x * d.accel.x + d.accel.y * d.accel.y + d.accel.z * d.accel.z);
        const float diff = fabsf(mag - 1.0f);
        shakeIntegrator = shakeIntegrator * 0.85f + diff;
        if (shakeIntegrator > 4.0f && now - lastShakeEventMs > 800) {
            lastShakeEventMs = now;
            pushEvent("imu.shake", "");
            shakeIntegrator = 0;
        }
        if (d.accel.z < -0.6f && now - lastShakeEventMs > 800) {
            pushEvent("imu.face_down", "");
            lastShakeEventMs = now;
        }
    }

    // Head-tap detection: rising edge of capacitive intensity on ANY zone.
    // Way more forgiving than `wasClicked()` — fires the instant you touch
    // any part of the head (no need for a precise "click" gesture). Held
    // touches don't repeat. 300 ms debounce prevents double-fires.
    auto& ts = M5StackChan.TouchSensor;
    static bool headTouched = false;
    static uint32_t lastHeadEdgeMs = 0;
    const auto& intens = ts.getIntensities();
    const bool nowTouched = (intens[0] > 0 || intens[1] > 0 || intens[2] > 0);
    if (nowTouched && !headTouched && (now - lastHeadEdgeMs) > 300) {
        lastHeadEdgeMs = now;
        char buf[64];
        snprintf(buf, sizeof(buf), "intensities=%u,%u,%u",
                 intens[0], intens[1], intens[2]);
        pushEvent("head.tap", buf);
        lastTouchEventMs = now;
        // Single touch toggles mic streaming (start/stop the conversation).
        micStreamToggle();
    }
    headTouched = nowTouched;
    if (ts.wasSwipedForward()) {
        pushEvent("head.swipe", "dir=forward");
    }
    if (ts.wasSwipedBackward()) {
        pushEvent("head.swipe", "dir=backward");
    }

    auto touch = M5.Touch.getDetail();
    if (touch.wasClicked()) {
        char buf[32];
        snprintf(buf, sizeof(buf), "x=%d y=%d", touch.x, touch.y);
        pushEvent("screen.tap", buf);
        // (Screen tap during recording no longer stops the mic — stop is
        // strictly via head touch now, per UX request.)
    }

    if (M5.BtnPWR.wasClicked()) {
        pushEvent("btn.pwr", "click");
    }
    if (M5.BtnA.wasClicked()) pushEvent("btn.a", "click");
    if (M5.BtnB.wasClicked()) pushEvent("btn.b", "click");
    if (M5.BtnC.wasClicked()) pushEvent("btn.c", "click");
}

// =========================================================================
// Idle motion + breathing + face animation
// =========================================================================

void idleMotionTick()
{
    if (!idleMotionEnabled) return;
    const uint32_t now = millis();
    if (nextIdleMs == 0) {
        nextIdleMs = now + kIdleMinIntervalMs + (esp_random() % (kIdleMaxIntervalMs - kIdleMinIntervalMs));
        return;
    }
    if (now < nextIdleMs) return;
    nextIdleMs = now + kIdleMinIntervalMs + (esp_random() % (kIdleMaxIntervalMs - kIdleMinIntervalMs));
    const int dx = (esp_random() % 901) - 450;  // -450..450
    const int dy = 200 + (esp_random() % 401);  // 200..600
    moveHead(dx, dy, 350);
}

void breathingTick()
{
    if (!autoBreathingEnabled) return;
    const uint32_t now = millis();
    if (now - lastBreathMs < 60) return;
    lastBreathMs = now;
    breathPhase = (breathPhase + 1) % 100;
    // tiny pitch oscillation, ±20 servo units, super subtle
    static int baseY = -1;
    if (baseY < 0) baseY = currentY;
    const float t = (breathPhase / 100.0f) * 6.2831f;
    const int dy = static_cast<int>(sinf(t) * 20);
    M5StackChan.Motion.move(currentX, clampInt(baseY + dy, kHeadYMin, kHeadYMax), 200);
}

void faceAnimationTick()
{
    if (displayMode != DisplayMode::Face) return;
    const uint32_t now = millis();
    // Auto-blink every 3-6s
    if (!blinking && now - lastBlinkMs > 3000 + (esp_random() % 3000)) {
        blinking = true;
        blinkUntilMs = now + 130;
        lastBlinkMs = now;
        renderFace();
    } else if (blinking && now > blinkUntilMs) {
        blinking = false;
        renderFace();
    }
}

// =========================================================================
// Network
// =========================================================================

void connectNetwork()
{
    WiFi.mode(WIFI_STA);
    WiFi.setHostname(kHostName);

    if (String(STACKCHAN_WIFI_SSID).length() > 0) {
        WiFi.begin(STACKCHAN_WIFI_SSID, STACKCHAN_WIFI_PASSWORD);
        Serial.printf("Connecting to WiFi SSID=%s\n", STACKCHAN_WIFI_SSID);
        for (int i = 0; i < 120 && WiFi.status() != WL_CONNECTED; i++) {
            delay(250);
            Serial.print(".");
        }
        Serial.println();
    }

    if (WiFi.status() != WL_CONNECTED) {
        Serial.printf("WiFi connect failed, status=%d\n", WiFi.status());
        WiFi.mode(WIFI_AP);
        WiFi.softAP("StackChan-OpenClaw");
        Serial.printf("AP mode IP=%s\n", WiFi.softAPIP().toString().c_str());
    } else {
        Serial.printf("WiFi connected IP=%s\n", WiFi.localIP().toString().c_str());
        if (MDNS.begin(kHostName)) {
            MDNS.addService("http", "tcp", 80);
            Serial.println("mDNS ready: http://stackchan.local/");
        }
    }
}

// =========================================================================
// HTTP handlers
// =========================================================================

void handleOptions()
{
    sendCors();
    server.send(204);
}

void handleRoot()
{
    if (!requireAuth()) return;
    sendCors();
    String html = "<!doctype html><html><body><h1>StackChan v";
    html += kFirmwareVersion;
    html += "</h1><p>HTTP API ready. See /api for endpoints.</p></body></html>";
    server.send(200, "text/html; charset=utf-8", html);
}

void handleApiIndex()
{
    if (!requireAuth()) return;
    DynamicJsonDocument doc(2048);
    doc["ok"] = true;
    doc["firmware"] = kFirmwareVersion;
    JsonArray ep = doc.createNestedArray("endpoints");
    const char* endpoints[] = {
        "GET /status", "GET /api", "GET /battery", "GET /sensors", "GET /events",
        "POST /display", "POST /display/clear", "POST /display/face",
        "POST /display/big", "POST /display/progress", "POST /display/marquee",
        "POST /display/brightness", "POST /display/status",
        "POST /led", "POST /led/effect", "POST /led/off",
        "POST /head", "POST /head/normalized", "POST /head/rotate",
        "POST /head/stop", "POST /head/torque", "POST /head/calibrate",
        "POST /home", "POST /action",
        "POST /idle", "POST /breathing",
        "POST /speak", "GET /speak/status", "POST /speak/stop",
        "POST /mic/record", "GET /mic/status", "GET /mic/last", "POST /mic/stop",
        "POST /camera/init", "POST /camera/deinit", "GET /camera/status",
        "GET /camera/capture", "POST /camera/config", "POST /camera/preview",
        "POST /webhook", "GET /webhook",
        "POST /volume", "POST /reboot",
    };
    for (const char* s : endpoints) ep.add(s);
    sendJson(200, doc);
}

void handleStatus()
{
    if (!requireAuth()) return;
    DynamicJsonDocument doc(1024);
    doc["ok"] = true;
    doc["host"] = String(kHostName) + ".local";
    doc["ip"] = WiFi.localIP().toString();
    doc["wifi"] = WiFi.isConnected() ? WiFi.SSID() : "ap";
    doc["rssi"] = WiFi.RSSI();
    doc["openclaw"] = OPENCLAW_GATEWAY_URL;
    doc["auth"] = tokenConfigured();
    doc["firmware"] = kFirmwareVersion;
    doc["uptime_ms"] = millis();
    JsonObject head = doc.createNestedObject("head");
    head["x"] = currentX;
    head["y"] = currentY;
    JsonObject disp = doc.createNestedObject("display");
    disp["mode"] = static_cast<int>(displayMode);
    disp["title"] = lastTitle;
    disp["text"] = lastText;
    disp["face"] = faceExpression;
    JsonObject leds = doc.createNestedObject("led");
    leds["effect"] = static_cast<int>(ledEffect);
    leds["r"] = ledR;
    leds["g"] = ledG;
    leds["b"] = ledB;
    leds["speed_ms"] = ledSpeed;
    doc["idle"] = idleMotionEnabled;
    doc["breathing"] = autoBreathingEnabled;
    doc["webhook"] = webhookUrl;
    bool playing = false;
    uint32_t pid = 0;
    String url, title;
    speakGetState(playing, pid, url, title);
    JsonObject spk = doc.createNestedObject("speak");
    spk["playing"] = playing;
    spk["id"] = pid;
    spk["title"] = title;
    sendJson(200, doc);
}

void handleBattery()
{
    if (!requireAuth()) return;
    StaticJsonDocument<200> doc;
    doc["ok"] = true;
    doc["voltage"] = M5StackChan.getBatteryVoltage();
    doc["current"] = M5StackChan.getBatteryCurrent();
    doc["level"] = M5.Power.getBatteryLevel();
    doc["charging"] = M5.Power.isCharging();
    sendJson(200, doc);
}

void handleSensors()
{
    if (!requireAuth()) return;
    DynamicJsonDocument doc(1024);
    doc["ok"] = true;
    doc["uptime_ms"] = millis();
    JsonObject imu = doc.createNestedObject("imu");
    JsonObject acc = imu.createNestedObject("accel");
    acc["x"] = lastAccelX; acc["y"] = lastAccelY; acc["z"] = lastAccelZ;
    JsonObject gyr = imu.createNestedObject("gyro");
    gyr["x"] = lastGyroX; gyr["y"] = lastGyroY; gyr["z"] = lastGyroZ;
    if (hasMagnetometer) {
        JsonObject mag = imu.createNestedObject("mag");
        mag["x"] = lastMagX; mag["y"] = lastMagY; mag["z"] = lastMagZ;
        // simple heading in degrees from raw magnetic field (no tilt comp)
        float heading = atan2f(lastMagY, lastMagX) * 180.0f / 3.14159f;
        if (heading < 0) heading += 360.0f;
        imu["heading_deg"] = heading;
    }
    JsonObject cam = doc.createNestedObject("camera");
    cam["enabled"] = cameraEnabled;
    cam["frame_size"] = frameSizeName(cameraFrameSize);
    cam["quality"] = cameraQuality;
    cam["last_bytes"] = cameraLastBytes;
    JsonObject touch = doc.createNestedObject("head_touch");
    const auto& intens = M5StackChan.TouchSensor.getIntensities();
    touch["front"] = intens[0];
    touch["middle"] = intens[1];
    touch["back"] = intens[2];
    JsonObject screen = doc.createNestedObject("screen_touch");
    auto td = M5.Touch.getDetail();
    screen["touched"] = td.isPressed();
    screen["x"] = td.x;
    screen["y"] = td.y;
    JsonObject batt = doc.createNestedObject("battery");
    batt["voltage"] = M5StackChan.getBatteryVoltage();
    batt["level"] = M5.Power.getBatteryLevel();
    batt["charging"] = M5.Power.isCharging();
    JsonObject head = doc.createNestedObject("head");
    head["x"] = currentX;
    head["y"] = currentY;
    head["moving"] = M5StackChan.Motion.isMoving();
    sendJson(200, doc);
}

void handleEvents()
{
    if (!requireAuth()) return;
    const uint32_t since = strtoul(server.arg("since").c_str(), nullptr, 10);
    DynamicJsonDocument doc(4096);
    doc["ok"] = true;
    JsonArray arr = doc.createNestedArray("events");
    if (eventMutex && xSemaphoreTake(eventMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        for (uint32_t i = 0; i < eventCount; i++) {
            const Event& e = eventRing[(eventHead + i) % kEventCapacity];
            if (e.id <= since) continue;
            JsonObject o = arr.createNestedObject();
            o["id"] = e.id;
            o["ts"] = e.ts;
            o["type"] = e.type;
            if (e.data[0]) o["data"] = e.data;
        }
        doc["latest"] = nextEventId > 0 ? nextEventId - 1 : 0;
        xSemaphoreGive(eventMutex);
    }
    sendJson(200, doc);
}

void handleDisplay()
{
    if (!requireAuth()) return;
    StaticJsonDocument<1024> doc;
    if (!parseBody(doc)) return;
    lastTitle = doc["title"] | "OpenClaw";
    lastText = doc["text"] | "";
    if (lastText.length() == 0) { sendError(400, "text is required"); return; }
    drawMessage();
    sendOk();
}

void handleDisplayClear()
{
    if (!requireAuth()) return;
    displayClear();
    sendOk();
}

void handleDisplayFace()
{
    if (!requireAuth()) return;
    StaticJsonDocument<512> doc;
    if (!parseBody(doc, true)) return;
    const String expr = doc["expression"] | "neutral";
    const uint32_t eye = parseHexColor(doc["eye_color"] | "", 0x00CED1);
    const uint32_t mouth = parseHexColor(doc["mouth_color"] | "", 0xFFA0B4);
    const uint32_t bg = parseHexColor(doc["bg"] | "", 0x000000);
    drawFace(expr, eye, mouth, bg);
    sendOk();
}

void handleDisplayBig()
{
    if (!requireAuth()) return;
    StaticJsonDocument<512> doc;
    if (!parseBody(doc)) return;
    const String text = doc["text"] | "";
    if (text.length() == 0) { sendError(400, "text is required"); return; }
    const uint32_t color = parseHexColor(doc["color"] | "", 0xFFFFFF);
    displayBig(text, color);
    sendOk();
}

void handleDisplayProgress()
{
    if (!requireAuth()) return;
    StaticJsonDocument<512> doc;
    if (!parseBody(doc)) return;
    progressPercent = clampInt(doc["percent"] | 0, 0, 100);
    progressLabel = String(doc["label"] | "");
    displayProgress();
    sendOk();
}

void handleDisplayMarquee()
{
    if (!requireAuth()) return;
    StaticJsonDocument<512> doc;
    if (!parseBody(doc)) return;
    marqueeText = String(doc["text"] | "");
    if (marqueeText.length() == 0) { sendError(400, "text is required"); return; }
    marqueeStepMs = clampInt(doc["step_ms"] | 40, 10, 500);
    marqueeColor = parseHexColor(doc["color"] | "", 0xFFFFFF);
    marqueePixelOffset = 0;
    lastMarqueeMs = millis();
    displayMode = DisplayMode::Marquee;
    sendOk();
}

void handleDisplayBrightness()
{
    if (!requireAuth()) return;
    StaticJsonDocument<128> doc;
    if (!parseBody(doc)) return;
    const int v = clampInt(doc["value"] | 200, 0, 255);
    M5.Display.setBrightness(v);
    sendOk();
}

void handleDisplayStatus()
{
    if (!requireAuth()) return;
    drawStatusScreen();
    sendOk();
}

void handleHead()
{
    if (!requireAuth()) return;
    StaticJsonDocument<256> doc;
    if (!parseBody(doc)) return;
    const int x = doc["x"] | currentX;
    const int y = doc["y"] | currentY;
    const int speed = doc["speed"] | 500;
    moveHead(x, y, speed);
    sendOk();
}

void handleHeadNormalized()
{
    if (!requireAuth()) return;
    StaticJsonDocument<256> doc;
    if (!parseBody(doc)) return;
    const float nx = doc["x"] | 0.0f;
    const float ny = doc["y"] | 0.0f;
    const int speed = doc["speed"] | 500;
    actionTrack(nx, ny, speed);
    sendOk();
}

void handleHeadRotate()
{
    if (!requireAuth()) return;
    StaticJsonDocument<256> doc;
    if (!parseBody(doc)) return;
    const int v = clampInt(doc["velocity"] | 0, -1000, 1000);
    M5StackChan.Motion.rotateYaw(v);
    sendOk();
}

void handleHeadStop()
{
    if (!requireAuth()) return;
    M5StackChan.Motion.stop();
    sendOk();
}

void handleHeadTorque()
{
    if (!requireAuth()) return;
    StaticJsonDocument<128> doc;
    if (!parseBody(doc, true)) return;
    const bool enabled = doc["enabled"] | true;
    M5StackChan.Motion.setTorqueEnabled(enabled);
    M5StackChan.setServoPowerEnabled(enabled);
    sendOk();
}

void handleHeadCalibrate()
{
    if (!requireAuth()) return;
    M5StackChan.Motion.setCurrentPostionAsHome();
    currentX = 0;
    currentY = 0;
    sendOk();
}

void handleHome()
{
    if (!requireAuth()) return;
    actionHome();
    sendOk();
}

void handleLed()
{
    if (!requireAuth()) return;
    StaticJsonDocument<256> doc;
    if (!parseBody(doc)) return;
    const uint8_t r = clampInt(doc["r"] | 0, 0, 255);
    const uint8_t g = clampInt(doc["g"] | 0, 0, 255);
    const uint8_t b = clampInt(doc["b"] | 0, 0, 255);
    if (doc["index"].is<int>()) {
        const int index = clampInt(doc["index"], 0, 11);
        ledEffect = LedEffect::Solid;  // individual writes pause effects
        M5StackChan.setRgbColor(index, r, g, b);
        M5StackChan.refreshRgb();
    } else {
        setLedEffect(LedEffect::Solid, r, g, b, 80);
    }
    sendOk();
}

void handleLedEffect()
{
    if (!requireAuth()) return;
    StaticJsonDocument<256> doc;
    if (!parseBody(doc)) return;
    const String name = doc["name"] | "solid";
    const uint8_t r = clampInt(doc["r"] | 0, 0, 255);
    const uint8_t g = clampInt(doc["g"] | 0, 0, 255);
    const uint8_t b = clampInt(doc["b"] | 0, 0, 255);
    const uint16_t spd = clampInt(doc["speed_ms"] | 80, 10, 2000);

    LedEffect eff = LedEffect::Solid;
    if (name == "off") eff = LedEffect::Off;
    else if (name == "solid") eff = LedEffect::Solid;
    else if (name == "rainbow") eff = LedEffect::Rainbow;
    else if (name == "breathing") eff = LedEffect::Breathing;
    else if (name == "pulse") eff = LedEffect::Pulse;
    else if (name == "scanner") eff = LedEffect::Scanner;
    else if (name == "wipe") eff = LedEffect::Wipe;
    else if (name == "sparkle") eff = LedEffect::Sparkle;
    else if (name == "police") eff = LedEffect::Police;
    else if (name == "fire") eff = LedEffect::Fire;
    else if (name == "chase") eff = LedEffect::Chase;
    else if (name == "theater") eff = LedEffect::Theater;
    else if (name == "listening") eff = LedEffect::Listening;
    else if (name == "thinking") eff = LedEffect::Thinking;
    else if (name == "talking") eff = LedEffect::Talking;
    else if (name == "recording") eff = LedEffect::Recording;
    else { sendError(400, "unknown led effect"); return; }

    setLedEffect(eff, r, g, b, spd);
    sendOk();
}

void handleLedOff()
{
    if (!requireAuth()) return;
    setLedEffect(LedEffect::Off, 0, 0, 0, 80);
    sendOk();
}

void handleAction()
{
    if (!requireAuth()) return;
    StaticJsonDocument<256> doc;
    if (!parseBody(doc)) return;
    const String name = doc["name"] | "";
    if      (name == "home")        actionHome();
    else if (name == "nod")         actionNod();
    else if (name == "shake")       actionShake();
    else if (name == "yes")         actionYes();
    else if (name == "no")          actionNo();
    else if (name == "left")        moveHead(-600, currentY, 500);
    else if (name == "right")       moveHead(600, currentY, 500);
    else if (name == "look_up")     moveHead(currentX, 700, 500);
    else if (name == "look_down")   moveHead(currentX, 200, 500);
    else if (name == "look_around") actionLookAround();
    else if (name == "dance")       actionDance();
    else if (name == "surprised")   actionSurprised();
    else if (name == "sleep")       actionSleep();
    else if (name == "wake")        actionWake();
    else if (name == "panic")       actionPanic();
    else if (name == "peek")        actionPeek();
    else if (name == "tilt_left")   actionTilt(true);
    else if (name == "tilt_right")  actionTilt(false);
    else if (name == "bow")         actionBow();
    else { sendError(400, "unknown action"); return; }
    sendOk();
}

void handleIdle()
{
    if (!requireAuth()) return;
    StaticJsonDocument<128> doc;
    if (!parseBody(doc, true)) return;
    idleMotionEnabled = doc["enabled"] | true;
    if (idleMotionEnabled) nextIdleMs = millis() + 1500;
    sendOk();
}

void handleBreathing()
{
    if (!requireAuth()) return;
    StaticJsonDocument<128> doc;
    if (!parseBody(doc, true)) return;
    autoBreathingEnabled = doc["enabled"] | true;
    sendOk();
}

void handleSpeak()
{
    if (!requireAuth()) return;
    StaticJsonDocument<512> doc;
    if (!parseBody(doc)) return;
    const String url = doc["url"] | "";
    if (url.length() == 0) { sendError(400, "url is required"); return; }
    if (url.length() >= sizeof(SpeakRequest::url)) { sendError(400, "url too long"); return; }

    speakStopRequested = true;
    M5.Speaker.stop();
    SpeakRequest drain;
    while (xQueueReceive(speakQueue, &drain, 0) == pdTRUE) {}

    SpeakRequest req = {};
    strlcpy(req.url, url.c_str(), sizeof(req.url));
    const String title = doc["title"] | "";
    if (title.length() > 0) {
        strlcpy(req.title, title.c_str(), sizeof(req.title));
        req.hasTitle = true;
    }
    req.id = speakNextId++;

    if (xQueueSend(speakQueue, &req, pdMS_TO_TICKS(200)) != pdTRUE) {
        sendError(503, "speak queue full");
        return;
    }
    StaticJsonDocument<160> resp;
    resp["ok"] = true;
    resp["id"] = req.id;
    sendJson(202, resp);
}

void handleSpeakStatus()
{
    if (!requireAuth()) return;
    bool playing = false;
    uint32_t id = 0;
    String url, title;
    speakGetState(playing, id, url, title);
    StaticJsonDocument<512> doc;
    doc["ok"] = true;
    doc["playing"] = playing;
    doc["id"] = id;
    doc["url"] = url;
    doc["title"] = title;
    sendJson(200, doc);
}

void handleSpeakStop()
{
    if (!requireAuth()) return;
    speakStopRequested = true;
    M5.Speaker.stop();
    SpeakRequest drain;
    while (xQueueReceive(speakQueue, &drain, 0) == pdTRUE) {}
    sendOk("stopped");
}

void handleMicRecord()
{
    if (!requireAuth()) return;
    StaticJsonDocument<512> doc;
    if (!parseBody(doc, true)) return;
    MicRequest req = {};
    req.id = micNextId++;
    req.seconds = clampInt(doc["seconds"] | 4, 1, kMicMaxSeconds);
    req.sample_rate = clampInt(doc["sample_rate"] | static_cast<int>(kMicDefaultSampleRate), 8000, 48000);
    const String url = doc["upload_url"] | "";
    if (url.length() > 0 && url.length() < sizeof(req.upload_url)) {
        req.upload = true;
        strlcpy(req.upload_url, url.c_str(), sizeof(req.upload_url));
    }
    const String prompt = doc["prompt"] | "";
    if (prompt.length() > 0) {
        strlcpy(req.prompt, prompt.c_str(), sizeof(req.prompt));
    }
    if (xQueueSend(micQueue, &req, pdMS_TO_TICKS(50)) != pdTRUE) {
        sendError(503, "mic busy");
        return;
    }
    StaticJsonDocument<200> resp;
    resp["ok"] = true;
    resp["id"] = req.id;
    resp["seconds"] = req.seconds;
    resp["upload"] = req.upload;
    sendJson(202, resp);
}

void handleMicStatus()
{
    if (!requireAuth()) return;
    StaticJsonDocument<200> doc;
    doc["ok"] = true;
    doc["recording"] = micRecording;
    doc["id"] = micRecordingId;
    doc["last_bytes"] = (uint32_t)micLastWavLen;
    doc["last_note"] = micLastNote;
    sendJson(200, doc);
}

void handleMicStop()
{
    if (!requireAuth()) return;
    micStopRequested = true;
    sendOk("stopping");
}

void handleMicLast()
{
    if (!requireAuth()) return;
    if (micBufMutex == nullptr || xSemaphoreTake(micBufMutex, pdMS_TO_TICKS(50)) != pdTRUE) {
        sendError(503, "mic busy"); return;
    }
    if (micLastWav == nullptr || micLastWavLen == 0) {
        xSemaphoreGive(micBufMutex);
        sendError(404, "no recording");
        return;
    }
    sendCors();
    server.sendHeader("Content-Type", "audio/wav");
    server.setContentLength(micLastWavLen);
    server.send(200, "audio/wav", "");
    WiFiClient client = server.client();
    const size_t chunk = 4096;
    size_t sent = 0;
    while (sent < micLastWavLen && client.connected()) {
        const size_t n = std::min(chunk, micLastWavLen - sent);
        const size_t written = client.write(micLastWav + sent, n);
        if (written == 0) break;
        sent += written;
    }
    xSemaphoreGive(micBufMutex);
}

void handleCameraInit()
{
    if (!requireAuth()) return;
    StaticJsonDocument<256> doc;
    parseBody(doc, true);
    const String size = doc["frame_size"] | "";
    if (size.length() > 0) cameraFrameSize = parseFrameSize(size, cameraFrameSize);
    if (doc["quality"].is<int>()) cameraQuality = clampInt(doc["quality"], 0, 63);
    if (cameraMutex && xSemaphoreTake(cameraMutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
        sendError(503, "camera busy"); return;
    }
    if (cameraEnabled) cameraDeinit();
    const bool ok = cameraInit();
    if (ok) cameraDeinit();   // probe only — camera is initialised per-capture, OFF between
    if (cameraMutex) xSemaphoreGive(cameraMutex);
    StaticJsonDocument<256> resp;
    resp["ok"] = ok;
    resp["enabled"] = cameraEnabled;
    resp["frame_size"] = frameSizeName(cameraFrameSize);
    resp["quality"] = cameraQuality;
    if (!ok) resp["error"] = cameraInitError;
    sendJson(ok ? 200 : 500, resp);
}

void handleCameraDeinit()
{
    if (!requireAuth()) return;
    if (cameraMutex && xSemaphoreTake(cameraMutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
        sendError(503, "camera busy"); return;
    }
    cameraDeinit();
    if (cameraMutex) xSemaphoreGive(cameraMutex);
    sendOk("deinit");
}

void handleCameraStatus()
{
    if (!requireAuth()) return;
    StaticJsonDocument<320> doc;
    doc["ok"] = true;
    doc["enabled"] = cameraEnabled;
    doc["init_tried"] = cameraInitTried;
    doc["frame_size"] = frameSizeName(cameraFrameSize);
    doc["quality"] = cameraQuality;
    doc["last_capture_ms"] = cameraLastCaptureMs;
    doc["last_bytes"] = cameraLastBytes;
    if (cameraInitError.length() > 0) doc["error"] = cameraInitError;
    sendJson(200, doc);
}

void handleCameraCapture()
{
    if (!requireAuth()) return;

    // All camera work runs on cameraTask; this handler only relays the result.
    // 5s cap: a healthy QVGA capture+encode is well under 400ms — anything near
    // 5s means the camera is genuinely stuck (see the cameraRunJob timeout note).
    uint8_t* jpg = nullptr;
    size_t jpgLen = 0;
    uint32_t capMs = 0;
    char err[48] = {0};
    if (!cameraRunJob(&jpg, &jpgLen, &capMs, err, sizeof(err), 5000)) {
        sendError(503, err[0] ? err : "camera error");
        return;
    }

    sendCors();
    server.sendHeader("Content-Disposition", "inline; filename=\"stackchan.jpg\"");
    server.sendHeader("Connection", "close");
    server.setContentLength(jpgLen);
    server.send(200, "image/jpeg", "");
    WiFiClient client = server.client();
    client.setTimeout(2);
    const size_t mss = 1436;
    size_t sent = 0;
    const uint32_t sendDeadline = millis() + 8000;   // hard cap 8s total
    int zeroStreak = 0;
    while (sent < jpgLen && client.connected() && millis() < sendDeadline) {
        const size_t want = std::min(mss, jpgLen - sent);
        const size_t wrote = client.write(jpg + sent, want);
        if (wrote == 0) {
            if (++zeroStreak > 20) break;   // give up after ~100ms of stalls
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }
        zeroStreak = 0;
        sent += wrote;
        vTaskDelay(0);   // yield to WiFi task without re-entering WebServer
    }
    client.stop();   // explicitly close so handleClient() doesn't keep waiting
    Serial.printf("[camera] sent %u/%u bytes in %ums (heap=%u)\n",
                  (uint32_t)sent, (uint32_t)jpgLen, capMs, (uint32_t)ESP.getFreeHeap());
    free(jpg);

    char ev[80];
    snprintf(ev, sizeof(ev), "bytes=%u ms=%u heap=%u",
             (uint32_t)jpgLen, capMs, (uint32_t)ESP.getFreeHeap());
    pushEvent("camera.capture", ev);
}

void handleCameraConfig()
{
    if (!requireAuth()) return;
    StaticJsonDocument<512> doc;
    if (!parseBody(doc)) return;
    if (cameraMutex && xSemaphoreTake(cameraMutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
        sendError(503, "camera busy"); return;
    }
    if (!cameraEnabled && !cameraInit()) {
        if (cameraMutex) xSemaphoreGive(cameraMutex);
        sendError(503, cameraInitError.length() > 0 ? cameraInitError.c_str() : "camera unavailable");
        return;
    }
    sensor_t* s = esp_camera_sensor_get();
    if (!s) {
        if (cameraMutex) xSemaphoreGive(cameraMutex);
        sendError(500, "no sensor"); return;
    }
    if (doc["frame_size"].is<const char*>()) {
        cameraFrameSize = parseFrameSize(String(doc["frame_size"].as<const char*>()), cameraFrameSize);
        s->set_framesize(s, cameraFrameSize);
    }
    if (doc["quality"].is<int>())     { cameraQuality = clampInt(doc["quality"], 0, 63); s->set_quality(s, cameraQuality); }
    if (doc["brightness"].is<int>())  s->set_brightness(s, clampInt(doc["brightness"], -2, 2));
    if (doc["contrast"].is<int>())    s->set_contrast(s, clampInt(doc["contrast"], -2, 2));
    if (doc["saturation"].is<int>())  s->set_saturation(s, clampInt(doc["saturation"], -2, 2));
    if (doc["effect"].is<int>())      s->set_special_effect(s, clampInt(doc["effect"], 0, 6));
    if (doc["hmirror"].is<bool>())    s->set_hmirror(s, doc["hmirror"] ? 1 : 0);
    if (doc["vflip"].is<bool>())      s->set_vflip(s, doc["vflip"] ? 1 : 0);
    if (doc["gainceiling"].is<int>()) s->set_gainceiling(s, (gainceiling_t)clampInt(doc["gainceiling"], 0, 6));
    if (doc["awb"].is<bool>())        s->set_whitebal(s, doc["awb"] ? 1 : 0);
    if (doc["aec"].is<bool>())        s->set_exposure_ctrl(s, doc["aec"] ? 1 : 0);
    // NOTE: the camera is re-initialised fresh on every capture (it must stay OFF
    // between shots — see cameraTask). So sensor-level tweaks here are NOT
    // persistent; only frame_size / quality survive (they are globals applied by
    // cameraInit). Tuning brightness/contrast/etc per-session isn't supported.
    cameraDeinit();
    if (cameraMutex) xSemaphoreGive(cameraMutex);
    sendOk();
}

void handleCameraPreview()
{
    // Show last capture on the LCD as a side effect of any /camera/capture call
    // is more complex; instead, this fetches a fresh QVGA RGB565 frame straight
    // to the LCD. Useful for debugging the camera framing without a HTTP client.
    if (!requireAuth()) return;
    if (cameraMutex && xSemaphoreTake(cameraMutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
        sendError(503, "camera busy"); return;
    }
    if (!cameraEnabled && !cameraInit()) {
        if (cameraMutex) xSemaphoreGive(cameraMutex);
        sendError(503, "camera unavailable"); return;
    }
    sensor_t* s = esp_camera_sensor_get();
    const framesize_t origSize = cameraFrameSize;
    if (s) s->set_framesize(s, FRAMESIZE_QVGA);
    camera_fb_t* fb = esp_camera_fb_get();
    if (fb && fb->format == PIXFORMAT_RGB565) {
        auto& d = M5StackChan.Display();
        d.startWrite();
        d.pushImage(0, 0, fb->width, fb->height, reinterpret_cast<uint16_t*>(fb->buf));
        d.endWrite();
        displayMode = DisplayMode::Clear;
    }
    if (fb) esp_camera_fb_return(fb);
    if (s) s->set_framesize(s, origSize);
    cameraDeinit();   // camera stays OFF between captures
    if (cameraMutex) xSemaphoreGive(cameraMutex);
    sendOk();
}

void handleWebhookSet()
{
    if (!requireAuth()) return;
    StaticJsonDocument<256> doc;
    if (!parseBody(doc, true)) return;
    const String url = doc["url"] | "";
    if (webhookCfgMutex && xSemaphoreTake(webhookCfgMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        webhookUrl = url;
        xSemaphoreGive(webhookCfgMutex);
    }
    sendOk();
}

void handleWebhookGet()
{
    if (!requireAuth()) return;
    StaticJsonDocument<256> doc;
    doc["ok"] = true;
    doc["url"] = webhookUrl;
    sendJson(200, doc);
}

void handleVolume()
{
    if (!requireAuth()) return;
    StaticJsonDocument<128> doc;
    if (!parseBody(doc)) return;
    const int v = clampInt(doc["value"] | 180, 0, 255);
    M5.Speaker.setVolume(v);
    sendOk();
}

void handleReboot()
{
    if (!requireAuth()) return;
    sendOk("rebooting");
    delay(150);
    ESP.restart();
}

// =========================================================================
// dispatch — single source of truth for "what each method does"
//
// HTTP handlers above still call internal C++ helpers directly (for backward
// compat). dispatch() is the *new* entry point that the upcoming WebSocket
// client and the existing HTTP path can both call. PR1 only adds dispatch();
// PR7 (打磨) will optionally migrate HTTP handlers to use dispatch too.
//
// Conventions:
// - `method` is dotted: "display.face", "head.normalized", etc.
// - `params` is the parsed request body (or an empty JsonVariant for GET-style
//   methods like "status").
// - For methods that return data (status, sensors, events, speak/mic/camera
//   status, etc.), populate `result` with the response JSON object.
// - For command methods (head, action, led, face, etc.), leave `result`
//   empty — caller decides whether to wrap with `{ok:true}`.
// - On error, return false and put the message in `error`. Prefix with a
//   numeric HTTP-style code + ":" to control the response code, e.g.
//   `"404:no recording"`, `"503:camera busy"`. Default is 400.
// - Binary-bodied methods (mic.last, camera.capture) are NOT routed via
//   dispatch — they need direct stream access. dispatch returns 415:binary
//   if asked, so callers can detect and fall back to the HTTP path.
// =========================================================================

[[gnu::used]]
bool dispatch(const String& method, JsonVariantConst params,
              JsonDocument& result, String& error)
{
    // ----- read-only / status methods -----
    if (method == "status") {
        result["ok"] = true;
        result["host"] = String(kHostName) + ".local";
        result["ip"] = WiFi.localIP().toString();
        result["wifi"] = WiFi.isConnected() ? WiFi.SSID() : "ap";
        result["rssi"] = WiFi.RSSI();
        result["openclaw"] = OPENCLAW_GATEWAY_URL;
        result["auth"] = tokenConfigured();
        result["firmware"] = kFirmwareVersion;
        result["uptime_ms"] = millis();
        result["boot_count"] = bootCount;
        result["device_id"] = deviceId;
        auto ws = result.createNestedObject("ws");
        ws["bound"] = wsBound;
        ws["connected"] = wsConnected;
        ws["hello_acked"] = wsHelloSent;
        ws["host"] = STACKPROXY_WS_HOST;
        ws["port"] = (int)STACKPROXY_WS_PORT;
        auto h = result.createNestedObject("head");
        h["x"] = currentX; h["y"] = currentY;
        auto d = result.createNestedObject("display");
        d["mode"] = static_cast<int>(displayMode);
        d["title"] = lastTitle;
        d["text"] = lastText;
        d["face"] = faceExpression;
        auto l = result.createNestedObject("led");
        l["effect"] = static_cast<int>(ledEffect);
        l["r"] = ledR; l["g"] = ledG; l["b"] = ledB;
        l["speed_ms"] = ledSpeed;
        result["idle"] = idleMotionEnabled;
        result["breathing"] = autoBreathingEnabled;
        result["webhook"] = webhookUrl;
        bool playing = false; uint32_t pid = 0; String surl, stitle;
        speakGetState(playing, pid, surl, stitle);
        auto sp = result.createNestedObject("speak");
        sp["playing"] = playing; sp["id"] = pid; sp["title"] = stitle;
        return true;
    }
    if (method == "battery") {
        result["ok"] = true;
        result["voltage"] = M5StackChan.getBatteryVoltage();
        result["current"] = M5StackChan.getBatteryCurrent();
        result["level"] = M5.Power.getBatteryLevel();
        result["charging"] = M5.Power.isCharging();
        return true;
    }
    if (method == "sensors") {
        result["ok"] = true;
        result["uptime_ms"] = millis();
        auto imu = result.createNestedObject("imu");
        auto acc = imu.createNestedObject("accel");
        acc["x"] = lastAccelX; acc["y"] = lastAccelY; acc["z"] = lastAccelZ;
        auto gyr = imu.createNestedObject("gyro");
        gyr["x"] = lastGyroX; gyr["y"] = lastGyroY; gyr["z"] = lastGyroZ;
        if (hasMagnetometer) {
            auto m = imu.createNestedObject("mag");
            m["x"] = lastMagX; m["y"] = lastMagY; m["z"] = lastMagZ;
            float h = atan2f(lastMagY, lastMagX) * 180.0f / 3.14159f;
            if (h < 0) h += 360.0f;
            imu["heading_deg"] = h;
        }
        auto cam = result.createNestedObject("camera");
        cam["enabled"] = cameraEnabled;
        cam["frame_size"] = frameSizeName(cameraFrameSize);
        cam["quality"] = cameraQuality;
        cam["last_bytes"] = cameraLastBytes;
        auto touch = result.createNestedObject("head_touch");
        const auto& intens = M5StackChan.TouchSensor.getIntensities();
        touch["front"] = intens[0]; touch["middle"] = intens[1]; touch["back"] = intens[2];
        auto screen = result.createNestedObject("screen_touch");
        auto td = M5.Touch.getDetail();
        screen["touched"] = td.isPressed(); screen["x"] = td.x; screen["y"] = td.y;
        auto bat = result.createNestedObject("battery");
        bat["voltage"] = M5StackChan.getBatteryVoltage();
        bat["level"] = M5.Power.getBatteryLevel();
        bat["charging"] = M5.Power.isCharging();
        auto hh = result.createNestedObject("head");
        hh["x"] = currentX; hh["y"] = currentY; hh["moving"] = M5StackChan.Motion.isMoving();
        return true;
    }
    if (method == "events") {
        const uint32_t since = params["since"] | 0;
        result["ok"] = true;
        auto arr = result.createNestedArray("events");
        if (eventMutex && xSemaphoreTake(eventMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            for (uint32_t i = 0; i < eventCount; i++) {
                const Event& e = eventRing[(eventHead + i) % kEventCapacity];
                if (e.id <= since) continue;
                auto o = arr.createNestedObject();
                o["id"] = e.id; o["ts"] = e.ts; o["type"] = e.type;
                if (e.data[0]) o["data"] = e.data;
            }
            result["latest"] = nextEventId > 0 ? nextEventId - 1 : 0;
            xSemaphoreGive(eventMutex);
        }
        return true;
    }
    if (method == "api") {
        result["ok"] = true;
        result["firmware"] = kFirmwareVersion;
        auto ep = result.createNestedArray("methods");
        const char* methods[] = {
            "status", "battery", "sensors", "events", "api",
            "display", "display.clear", "display.face", "display.big",
            "display.progress", "display.marquee", "display.brightness", "display.status",
            "led", "led.effect", "led.off",
            "head", "head.normalized", "head.rotate", "head.stop",
            "head.torque", "head.calibrate", "home", "action",
            "idle", "breathing",
            "speak", "speak.status", "speak.stop",
            "mic.record", "mic.status", "mic.stop", "mic.last(binary)",
            "camera.init", "camera.deinit", "camera.status", "camera.capture(binary)",
            "camera.config", "camera.preview",
            "webhook.set", "webhook.get",
            "volume", "reboot",
        };
        for (const char* s : methods) ep.add(s);
        return true;
    }

    // ----- display -----
    if (method == "display") {
        const String t = params["text"] | "";
        if (t.length() == 0) { error = "text is required"; return false; }
        lastTitle = params["title"] | "OpenClaw";
        lastText = t;
        drawMessage();
        return true;
    }
    if (method == "display.clear") { displayClear(); return true; }
    if (method == "display.status") { drawStatusScreen(); return true; }
    if (method == "display.face") {
        const String expr = params["expression"] | "neutral";
        const String eyeS = params["eye_color"] | "";
        const String mouthS = params["mouth_color"] | "";
        const String bgS = params["bg"] | "";
        drawFace(expr,
                 parseHexColor(eyeS, 0x00CED1),
                 parseHexColor(mouthS, 0xFFA0B4),
                 parseHexColor(bgS, 0x000000));
        return true;
    }
    if (method == "display.big") {
        const String t = params["text"] | "";
        if (t.length() == 0) { error = "text is required"; return false; }
        const String colS = params["color"] | "";
        displayBig(t, parseHexColor(colS, 0xFFFFFF));
        return true;
    }
    if (method == "display.progress") {
        progressPercent = clampInt(params["percent"] | 0, 0, 100);
        progressLabel = String(params["label"] | "");
        displayProgress();
        return true;
    }
    if (method == "display.marquee") {
        const String t = params["text"] | "";
        if (t.length() == 0) { error = "text is required"; return false; }
        marqueeText = t;
        marqueeStepMs = clampInt(params["step_ms"] | 40, 10, 500);
        const String colS = params["color"] | "";
        marqueeColor = parseHexColor(colS, 0xFFFFFF);
        marqueePixelOffset = 0;
        lastMarqueeMs = millis();
        displayMode = DisplayMode::Marquee;
        return true;
    }
    if (method == "display.brightness") {
        const int v = clampInt(params["value"] | 200, 0, 255);
        M5.Display.setBrightness(v);
        return true;
    }

    // ----- led -----
    if (method == "led") {
        const uint8_t r = clampInt(params["r"] | 0, 0, 255);
        const uint8_t g = clampInt(params["g"] | 0, 0, 255);
        const uint8_t b = clampInt(params["b"] | 0, 0, 255);
        if (params["index"].is<int>()) {
            ledEffect = LedEffect::Solid;
            M5StackChan.setRgbColor(clampInt(params["index"], 0, 11), r, g, b);
            M5StackChan.refreshRgb();
        } else {
            setLedEffect(LedEffect::Solid, r, g, b, 80);
        }
        return true;
    }
    if (method == "led.effect") {
        const String name = params["name"] | "solid";
        const uint8_t r = clampInt(params["r"] | 0, 0, 255);
        const uint8_t g = clampInt(params["g"] | 0, 0, 255);
        const uint8_t b = clampInt(params["b"] | 0, 0, 255);
        const uint16_t spd = clampInt(params["speed_ms"] | 80, 10, 2000);
        LedEffect eff;
        if      (name == "off")        eff = LedEffect::Off;
        else if (name == "solid")      eff = LedEffect::Solid;
        else if (name == "rainbow")    eff = LedEffect::Rainbow;
        else if (name == "breathing")  eff = LedEffect::Breathing;
        else if (name == "pulse")      eff = LedEffect::Pulse;
        else if (name == "scanner")    eff = LedEffect::Scanner;
        else if (name == "wipe")       eff = LedEffect::Wipe;
        else if (name == "sparkle")    eff = LedEffect::Sparkle;
        else if (name == "police")     eff = LedEffect::Police;
        else if (name == "fire")       eff = LedEffect::Fire;
        else if (name == "chase")      eff = LedEffect::Chase;
        else if (name == "theater")    eff = LedEffect::Theater;
        else if (name == "listening")  eff = LedEffect::Listening;
        else if (name == "thinking")   eff = LedEffect::Thinking;
        else if (name == "talking")    eff = LedEffect::Talking;
        else if (name == "recording")  eff = LedEffect::Recording;
        else { error = "unknown led effect"; return false; }
        setLedEffect(eff, r, g, b, spd);
        return true;
    }
    if (method == "led.off") {
        setLedEffect(LedEffect::Off, 0, 0, 0, 80);
        return true;
    }

    // ----- head / motion -----
    if (method == "head") {
        const int x = params["x"] | currentX;
        const int y = params["y"] | currentY;
        const int speed = params["speed"] | 500;
        moveHead(x, y, speed);
        return true;
    }
    if (method == "head.normalized") {
        const float nx = params["x"] | 0.0f;
        const float ny = params["y"] | 0.0f;
        const int speed = params["speed"] | 500;
        actionTrack(nx, ny, speed);
        return true;
    }
    if (method == "head.rotate") {
        const int v = clampInt(params["velocity"] | 0, -1000, 1000);
        M5StackChan.Motion.rotateYaw(v);
        return true;
    }
    if (method == "head.stop") { M5StackChan.Motion.stop(); return true; }
    if (method == "head.torque") {
        const bool en = params["enabled"] | true;
        M5StackChan.Motion.setTorqueEnabled(en);
        M5StackChan.setServoPowerEnabled(en);
        return true;
    }
    if (method == "head.calibrate") {
        M5StackChan.Motion.setCurrentPostionAsHome();
        currentX = 0; currentY = 0;
        return true;
    }
    if (method == "home") { actionHome(); return true; }
    if (method == "action") {
        const String n = params["name"] | "";
        if      (n == "home")        actionHome();
        else if (n == "nod")         actionNod();
        else if (n == "shake")       actionShake();
        else if (n == "yes")         actionYes();
        else if (n == "no")          actionNo();
        else if (n == "left")        moveHead(-600, currentY, 500);
        else if (n == "right")       moveHead(600, currentY, 500);
        else if (n == "look_up")     moveHead(currentX, 700, 500);
        else if (n == "look_down")   moveHead(currentX, 200, 500);
        else if (n == "look_around") actionLookAround();
        else if (n == "dance")       actionDance();
        else if (n == "surprised")   actionSurprised();
        else if (n == "sleep")       actionSleep();
        else if (n == "wake")        actionWake();
        else if (n == "panic")       actionPanic();
        else if (n == "peek")        actionPeek();
        else if (n == "tilt_left")   actionTilt(true);
        else if (n == "tilt_right")  actionTilt(false);
        else if (n == "bow")         actionBow();
        else { error = "unknown action"; return false; }
        return true;
    }

    // ----- idle / breathing -----
    if (method == "idle") {
        idleMotionEnabled = params["enabled"] | true;
        if (idleMotionEnabled) nextIdleMs = millis() + 1500;
        return true;
    }
    if (method == "breathing") {
        autoBreathingEnabled = params["enabled"] | true;
        return true;
    }

    // ----- speak -----
    if (method == "speak") {
        const String url = params["url"] | "";
        if (url.length() == 0) { error = "url is required"; return false; }
        if (url.length() >= sizeof(SpeakRequest::url)) { error = "url too long"; return false; }
        speakStopRequested = true;
        M5.Speaker.stop();
        SpeakRequest drain;
        while (xQueueReceive(speakQueue, &drain, 0) == pdTRUE) {}
        SpeakRequest req = {};
        strlcpy(req.url, url.c_str(), sizeof(req.url));
        const String title = params["title"] | "";
        if (title.length() > 0) {
            strlcpy(req.title, title.c_str(), sizeof(req.title));
            req.hasTitle = true;
        }
        req.id = speakNextId++;
        if (xQueueSend(speakQueue, &req, pdMS_TO_TICKS(200)) != pdTRUE) {
            error = "503:speak queue full"; return false;
        }
        result["ok"] = true;
        result["id"] = req.id;
        return true;
    }
    if (method == "speak.status") {
        bool playing = false; uint32_t id = 0; String url, title;
        speakGetState(playing, id, url, title);
        result["ok"] = true;
        result["playing"] = playing;
        result["id"] = id;
        result["url"] = url;
        result["title"] = title;
        return true;
    }
    if (method == "speak.stop") {
        speakStopRequested = true;
        M5.Speaker.stop();
        SpeakRequest drain;
        while (xQueueReceive(speakQueue, &drain, 0) == pdTRUE) {}
        return true;
    }

    // ----- mic -----
    if (method == "mic.stream.start") {
        micStreamStart();
        result["ok"] = true;
        result["streaming"] = micStreaming;
        return true;
    }
    if (method == "mic.stream.stop") {
        micStreamStop();
        result["ok"] = true;
        result["streaming"] = micStreaming;
        return true;
    }
    if (method == "mic.stream.toggle") {
        micStreamToggle();
        result["ok"] = true;
        result["streaming"] = micStreaming;
        return true;
    }
    if (method == "mic.record") {
        MicRequest req = {};
        req.id = micNextId++;
        req.seconds = clampInt(params["seconds"] | 4, 1, kMicMaxSeconds);
        req.sample_rate = clampInt(params["sample_rate"] | (int)kMicDefaultSampleRate, 8000, 48000);
        const String url = params["upload_url"] | "";
        if (url.length() > 0 && url.length() < sizeof(req.upload_url)) {
            req.upload = true;
            strlcpy(req.upload_url, url.c_str(), sizeof(req.upload_url));
        }
        const String prompt = params["prompt"] | "";
        if (prompt.length() > 0) {
            strlcpy(req.prompt, prompt.c_str(), sizeof(req.prompt));
        }
        if (xQueueSend(micQueue, &req, pdMS_TO_TICKS(50)) != pdTRUE) {
            error = "503:mic busy"; return false;
        }
        result["ok"] = true;
        result["id"] = req.id;
        result["seconds"] = req.seconds;
        result["upload"] = req.upload;
        return true;
    }
    if (method == "mic.status") {
        result["ok"] = true;
        result["recording"] = micRecording;
        result["id"] = micRecordingId;
        result["last_bytes"] = (uint32_t)micLastWavLen;
        result["last_note"] = micLastNote;
        return true;
    }
    if (method == "mic.stop") {
        micStopRequested = true;
        return true;
    }
    if (method == "mic.last") {
        error = "415:binary response, use HTTP /mic/last";
        return false;
    }

    // ----- camera -----
    if (method == "camera.init") {
        const String size = params["frame_size"] | "";
        if (size.length() > 0) cameraFrameSize = parseFrameSize(size, cameraFrameSize);
        if (params["quality"].is<int>()) cameraQuality = clampInt(params["quality"], 0, 63);
        if (cameraEnabled) cameraDeinit();
        const bool ok = cameraInit();
        if (ok) cameraDeinit();   // probe only — camera is initialised per-capture, OFF between
        result["ok"] = ok;
        result["enabled"] = cameraEnabled;
        result["frame_size"] = frameSizeName(cameraFrameSize);
        result["quality"] = cameraQuality;
        if (!ok) {
            result["error"] = cameraInitError;
            error = "500:" + cameraInitError;
            return false;
        }
        return true;
    }
    if (method == "camera.deinit") { cameraDeinit(); return true; }
    if (method == "camera.status") {
        result["ok"] = true;
        result["enabled"] = cameraEnabled;
        result["init_tried"] = cameraInitTried;
        result["frame_size"] = frameSizeName(cameraFrameSize);
        result["quality"] = cameraQuality;
        result["last_capture_ms"] = cameraLastCaptureMs;
        result["last_bytes"] = cameraLastBytes;
        if (cameraInitError.length() > 0) result["error"] = cameraInitError;
        return true;
    }
    if (method == "camera.capture") {
        error = "415:binary response, use HTTP /camera/capture";
        return false;
    }
    if (method == "camera.config") {
        if (!cameraEnabled && !cameraInit()) {
            error = "503:" + (cameraInitError.length() > 0 ? cameraInitError : String("camera unavailable"));
            return false;
        }
        sensor_t* s = esp_camera_sensor_get();
        if (!s) { error = "500:no sensor"; return false; }
        if (params["frame_size"].is<const char*>()) {
            cameraFrameSize = parseFrameSize(String(params["frame_size"].as<const char*>()), cameraFrameSize);
            s->set_framesize(s, cameraFrameSize);
        }
        if (params["quality"].is<int>())     { cameraQuality = clampInt(params["quality"], 0, 63); s->set_quality(s, cameraQuality); }
        if (params["brightness"].is<int>())  s->set_brightness(s, clampInt(params["brightness"], -2, 2));
        if (params["contrast"].is<int>())    s->set_contrast(s, clampInt(params["contrast"], -2, 2));
        if (params["saturation"].is<int>())  s->set_saturation(s, clampInt(params["saturation"], -2, 2));
        if (params["effect"].is<int>())      s->set_special_effect(s, clampInt(params["effect"], 0, 6));
        if (params["hmirror"].is<bool>())    s->set_hmirror(s, params["hmirror"] ? 1 : 0);
        if (params["vflip"].is<bool>())      s->set_vflip(s, params["vflip"] ? 1 : 0);
        if (params["gainceiling"].is<int>()) s->set_gainceiling(s, (gainceiling_t)clampInt(params["gainceiling"], 0, 6));
        if (params["awb"].is<bool>())        s->set_whitebal(s, params["awb"] ? 1 : 0);
        if (params["aec"].is<bool>())        s->set_exposure_ctrl(s, params["aec"] ? 1 : 0);
        cameraDeinit();   // sensor tweaks are not persistent — camera is OFF between shots
        return true;
    }
    if (method == "camera.preview") {
        if (!cameraEnabled && !cameraInit()) { error = "503:camera unavailable"; return false; }
        sensor_t* s = esp_camera_sensor_get();
        const framesize_t origSize = cameraFrameSize;
        if (s) s->set_framesize(s, FRAMESIZE_QVGA);
        camera_fb_t* fb = esp_camera_fb_get();
        if (fb && fb->format == PIXFORMAT_RGB565) {
            auto& d = M5StackChan.Display();
            d.startWrite();
            d.pushImage(0, 0, fb->width, fb->height, reinterpret_cast<uint16_t*>(fb->buf));
            d.endWrite();
            displayMode = DisplayMode::Clear;
        }
        if (fb) esp_camera_fb_return(fb);
        if (s) s->set_framesize(s, origSize);
        cameraDeinit();   // camera stays OFF between captures
        return true;
    }

    // ----- webhook / system -----
    if (method == "webhook.set") {
        const String url = params["url"] | "";
        if (webhookCfgMutex && xSemaphoreTake(webhookCfgMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
            webhookUrl = url;
            xSemaphoreGive(webhookCfgMutex);
        }
        return true;
    }
    if (method == "webhook.get") {
        result["ok"] = true;
        result["url"] = webhookUrl;
        return true;
    }
    if (method == "volume") {
        const int v = clampInt(params["value"] | 180, 0, 255);
        M5.Speaker.setVolume(v);
        return true;
    }
    if (method == "reboot") {
        // Caller is responsible for sending the response BEFORE we restart.
        // dispatch() returns immediately; reboot is scheduled on the loop.
        result["ok"] = true;
        result["message"] = "rebooting";
        // Schedule reboot 150 ms out so the HTTP response can be sent.
        // We set a flag the main loop will check; if main loop is too busy,
        // the deferred reboot fires from a one-shot task.
        xTaskCreate([](void*){ vTaskDelay(pdMS_TO_TICKS(150)); ESP.restart(); },
                    "reboot", 2048, nullptr, 1, nullptr);
        return true;
    }

    error = "unknown method: " + method;
    return false;
}

// =========================================================================
// WebSocket client (stackproxy long-connection, sub-protocol "ocsc.v1")
// =========================================================================

void wsLoadOrBumpBootCount()
{
    Preferences nvs;
    if (!nvs.begin(kNvsNamespace, false)) {
        bootCount = 1;
        return;
    }
    uint32_t prev = nvs.getUInt(kNvsKeyBoot, 0);
    bootCount = prev + 1;
    nvs.putUInt(kNvsKeyBoot, bootCount);
    nvs.end();
}

String wsMakeConnectionId()
{
    // 16 hex chars from esp_random — not a real UUID but plenty for ws session
    uint8_t bytes[8];
    for (int i = 0; i < 8; i++) bytes[i] = static_cast<uint8_t>(esp_random() & 0xFF);
    char buf[20];
    for (int i = 0; i < 8; i++) snprintf(buf + i * 2, 3, "%02x", bytes[i]);
    buf[16] = '\0';
    return String(buf);
}

String wsMakeDeviceId()
{
    String mac = WiFi.macAddress();    // e.g. "AA:BB:CC:DD:EE:FF"
    mac.replace(":", "");
    mac.toLowerCase();
    return "stackchan-" + mac;
}

// Enqueue a text frame to be sent from the main loop. Safe to call from any
// task. Drops oldest if queue full so we never block.
void wsEnqueueText(const String& payload)
{
    if (wsOutQueue == nullptr) return;
    WsOutMsg msg{};
    strlcpy(msg.buf, payload.c_str(), sizeof(msg.buf));
    if (xQueueSend(wsOutQueue, &msg, 0) != pdTRUE) {
        WsOutMsg drop;
        xQueueReceive(wsOutQueue, &drop, 0);
        xQueueSend(wsOutQueue, &msg, 0);
    }
}

// Enqueue a binary frame (8-byte ocsc.v1 header + PCM). The buffer must be
// heap-allocated; the main loop will free it after sendBIN. On overflow the
// oldest binary is dropped.
void wsEnqueueBin(uint8_t* heapData, size_t length)
{
    if (wsOutBinQueue == nullptr || heapData == nullptr) { free(heapData); return; }
    WsOutBin item = { heapData, length };
    if (xQueueSend(wsOutBinQueue, &item, 0) != pdTRUE) {
        WsOutBin drop;
        if (xQueueReceive(wsOutBinQueue, &drop, 0) == pdTRUE) {
            free(drop.data);
        }
        if (xQueueSend(wsOutBinQueue, &item, 0) != pdTRUE) {
            free(heapData);
        }
    }
}

// Pack a binary frame: 8-byte header + payload. Returns heap-allocated buf
// (caller must arrange free via wsEnqueueBin or free()).
uint8_t* wsPackBinaryFrame(uint8_t kind, uint16_t sid, uint32_t seq,
                            const void* payload, size_t pcmLen, size_t* outLen)
{
    size_t total = 8 + pcmLen;
    uint8_t* buf = static_cast<uint8_t*>(malloc(total));
    if (buf == nullptr) { if (outLen) *outLen = 0; return nullptr; }
    buf[0] = 0x01;   // PROTOCOL_VERSION
    buf[1] = kind;
    writeUint16LE(buf + 2, sid);
    writeUint32LE(buf + 4, seq);
    memcpy(buf + 8, payload, pcmLen);
    if (outLen) *outLen = total;
    return buf;
}

void wsSendHello()
{
    StaticJsonDocument<512> doc;
    doc["t"] = "hello";
    doc["device_id"] = deviceId;
    doc["fw"] = kFirmwareVersion;
    doc["boot_count"] = bootCount;
    doc["connection_id"] = connectionId;
    auto caps = doc.createNestedArray("caps");
    caps.add("display");
    caps.add("display.face");
    caps.add("led");
    caps.add("led.effect");
    caps.add("head");
    caps.add("action");
    caps.add("speak");
    caps.add("mic");
    caps.add("sensors");
    caps.add("camera");  // available via HTTP (not ws — see plan)
    String payload;
    serializeJson(doc, payload);
    wsEnqueueText(payload);
}

void wsSendRes(const String& rpcId, const JsonDocument& result)
{
    StaticJsonDocument<3072> out;
    out["t"] = "res";
    out["id"] = rpcId;
    out["ok"] = true;
    if (!result.isNull() && result.size() > 0) {
        out["d"] = result.as<JsonVariantConst>();
    } else {
        out.createNestedObject("d");
    }
    String payload;
    serializeJson(out, payload);
    wsEnqueueText(payload);
}

void wsSendErr(const String& rpcId, int code, const String& msg)
{
    StaticJsonDocument<256> out;
    out["t"] = "err";
    out["id"] = rpcId;
    char codebuf[16];
    snprintf(codebuf, sizeof(codebuf), "%d", code);
    out["code"] = codebuf;
    out["msg"] = msg;
    String payload;
    serializeJson(out, payload);
    wsEnqueueText(payload);
}

void wsSendEvt(const char* name, const char* data)
{
    StaticJsonDocument<384> out;
    out["t"] = "evt";
    out["name"] = name;
    if (data && data[0]) {
        out["d"] = data;
    }
    String payload;
    serializeJson(out, payload);
    wsEnqueueText(payload);
}

void wsHandleReq(const JsonDocument& msg)
{
    String rpcId = msg["id"].as<const char*>() ? msg["id"].as<String>() : "0";
    String method = msg["m"].as<const char*>() ? msg["m"].as<String>() : "";
    if (method.length() == 0) {
        wsSendErr(rpcId, 400, "missing method");
        return;
    }
    JsonVariantConst params = msg["p"];
    DynamicJsonDocument result(2048);
    String error;
    bool ok = dispatch(method, params, result, error);
    if (!ok) {
        // Errors may be prefixed with "NNN:" to control status code
        int code = 400;
        String emsg = error;
        int colon = error.indexOf(':');
        if (colon > 0 && colon < 5) {
            String head = error.substring(0, colon);
            bool isNum = head.length() > 0;
            for (size_t i = 0; i < head.length(); i++) {
                if (!isdigit(head[i])) { isNum = false; break; }
            }
            if (isNum) {
                code = head.toInt();
                emsg = error.substring(colon + 1);
                emsg.trim();
            }
        }
        wsSendErr(rpcId, code, emsg);
        return;
    }
    wsSendRes(rpcId, result);
}

// ---- ws TTS streaming state (PR6) ----
volatile uint16_t wsTtsSid = 0;
volatile uint32_t wsTtsSampleRate = 16000;
volatile bool wsTtsActive = false;
uint32_t wsTtsBytesPlayed = 0;
uint32_t wsTtsLastChunkMs = 0;
String wsTtsFacePrev;
// Accumulating PCM buffer for current TTS session — populated by binary
// frames, played in one shot at tts.end. Per-chunk M5.Speaker.playRaw calls
// were the source of click/pop at chunk boundaries.
uint8_t* wsTtsBuf = nullptr;
size_t wsTtsBufCap = 0;
size_t wsTtsBufLen = 0;
constexpr size_t kWsTtsBufMaxBytes = 4 * 1024 * 1024;  // 4 MB ≈ 2 minutes

// Whenever any phase of the convo advances (tts.start arriving, tts.done),
// we extend or clear the watchdog so it only fires on a true hang.
void convoCancelWatchdog() { convoWatchdogDeadlineMs = 0; }

void wsTtsBegin(uint16_t sid, uint32_t sr, uint32_t est_ms)
{
    if (wsTtsActive) {
        // Same sid? maybe a re-arm. Different sid? stop previous.
        if (wsTtsSid != sid) {
            M5.Speaker.stop();
        }
    }
    wsTtsSid = sid;
    wsTtsSampleRate = sr ? sr : 16000;
    wsTtsActive = true;
    wsTtsBytesPlayed = 0;
    wsTtsLastChunkMs = millis();
    wsTtsFacePrev = faceExpression;
    faceExpression = "talking";
    ledEffect = LedEffect::Talking;
    // TTS arriving = agent replied = conversation succeeded
    convoCancelWatchdog();
    // Allocate a buffer big enough for est_ms of audio + 25% slack, falling
    // back to 256 KB if no estimate was supplied (~8s of audio).
    if (wsTtsBuf) { free(wsTtsBuf); wsTtsBuf = nullptr; wsTtsBufCap = 0; }
    size_t cap = est_ms > 0
        ? (size_t)((uint64_t)est_ms * wsTtsSampleRate * 2 / 1000) * 5 / 4
        : 256 * 1024;
    if (cap > kWsTtsBufMaxBytes) cap = kWsTtsBufMaxBytes;
    // Prefer DRAM (INTERNAL) — same as the proven speakTask. DMA reads from
    // internal RAM are glitch-free; PSRAM is only a fallback for big buffers.
    wsTtsBuf = (uint8_t*)heap_caps_malloc(cap, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!wsTtsBuf) {
        wsTtsBuf = (uint8_t*)heap_caps_malloc(cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    wsTtsBufCap = wsTtsBuf ? cap : 0;
    wsTtsBufLen = 0;
    Serial.printf("[ws.tts] start sid=%u sr=%u est=%ums cap=%u\n",
                  (unsigned)sid, (unsigned)wsTtsSampleRate,
                  (unsigned)est_ms, (unsigned)wsTtsBufCap);
}

void wsTtsEnd()
{
    if (!wsTtsActive) return;
    wsTtsActive = false;
    // Single playRaw on the assembled PCM buffer — eliminates the
    // click/pop at every per-chunk boundary that the streaming approach
    // produced. M5.Speaker handles its own DMA pacing internally.
    if (wsTtsBuf && wsTtsBufLen >= 2) {
        Serial.printf("[ws.tts] end buffered=%u B (~%.1fs)\n",
                      (unsigned)wsTtsBufLen,
                      wsTtsBufLen / float(wsTtsSampleRate * 2));
        // Speaker was already re-armed in micStreamTask right after the
        // recording ended (see micStreamTask), so the I2S is in TX mode.
        // Just play the assembled buffer in one shot.
        M5.Speaker.playRaw(reinterpret_cast<const int16_t*>(wsTtsBuf),
                           wsTtsBufLen / 2, wsTtsSampleRate,
                           false, 1, 0);
        // Wait for it to drain so we can free the buffer cleanly
        uint32_t playDeadline = millis()
            + (wsTtsBufLen / float(wsTtsSampleRate * 2)) * 1000 + 2000;
        while (M5.Speaker.isPlaying() && millis() < playDeadline) {
            delay(30);
        }
    } else {
        Serial.println("[ws.tts] end with empty buffer");
    }
    if (wsTtsBuf) { free(wsTtsBuf); wsTtsBuf = nullptr; }
    wsTtsBufCap = 0;
    wsTtsBufLen = 0;
    // Restore face / LED and reset screen so user can tap again
    convoResetIdle("StackChan", "Ready");
    pushEvent("tts.done", "");
}

void wsHandleText(const char* payload, size_t length)
{
    DynamicJsonDocument doc(2048);
    DeserializationError err = deserializeJson(doc, payload, length);
    if (err) {
        Serial.printf("[ws] bad json: %s\n", err.c_str());
        return;
    }
    const char* type = doc["t"].as<const char*>();
    if (!type) return;
    String t(type);
    if (t == "req") {
        wsHandleReq(doc);
    } else if (t == "hello.ack") {
        wsHelloSent = true;
        Serial.println("[ws] hello.ack received");
    } else if (t == "ping") {
        StaticJsonDocument<96> out;
        out["t"] = "pong";
        out["ts"] = doc["ts"] | (uint32_t)millis();
        String payload;
        serializeJson(out, payload);
        wsEnqueueText(payload);
    } else if (t == "pong") {
        // ignore — heartbeat is library-managed
    } else if (t == "tts.start") {
        uint16_t sid = doc["sid"] | 0;
        uint32_t sr = doc["sr"] | 16000;
        uint32_t est_ms = doc["est_ms"] | 0;
        wsTtsBegin(sid, sr, est_ms);
    } else if (t == "tts.end") {
        wsTtsEnd();
    } else if (t == "evt") {
        // Server-originated event. For asr.partial / asr.final we update
        // the screen with the live transcript so the user sees what we
        // heard. Throttling already happens server-side (Doubao emits
        // partial only when text changes).
        const char* nm = doc["name"].as<const char*>();
        if (nm == nullptr) {
            // no-op
        } else if (strcmp(nm, "asr.partial") == 0) {
            const char* text = doc["d"]["text"].as<const char*>();
            if (text && text[0] != '\0') {
                lastTitle = "Listening";
                lastText = text;
                displayMode = DisplayMode::Message;
                displayNeedsRedraw = millis();
            }
        } else if (strcmp(nm, "asr.final") == 0) {
            const char* text = doc["d"]["text"].as<const char*>();
            const char* err = doc["d"]["error"].as<const char*>();
            if (err && err[0] != '\0') {
                // ASR failed → tell the user, reset state, cancel watchdog
                Serial.printf("[asr] err from server: %s\n", err);
                convoResetIdle("ASR error", err);
            } else if (text && text[0] != '\0') {
                lastTitle = "已收到，思考中...";
                lastText = text;
                displayMode = DisplayMode::Message;
                displayNeedsRedraw = millis();
                // Re-arm the watchdog FRESH here — the agent is only now
                // starting its (potentially 60s+) run, so the clock that
                // was armed at mic.end shouldn't count against it.
                convoWatchdogDeadlineMs = millis() + kConvoWatchdogMs;
            } else {
                // Empty final = nothing heard. Reset so user can retry.
                convoResetIdle("Didn't catch that", "Tap to try again");
            }
        } else if (strcmp(nm, "agent.error") == 0) {
            const char* err = doc["d"]["error"].as<const char*>();
            Serial.printf("[agent] err: %s\n", err ? err : "?");
            convoResetIdle("Agent error", err ? err : "Tap to retry");
        }
    } else if (t == "err") {
        Serial.printf("[ws] server err: code=%s msg=%s\n",
                      doc["code"].as<const char*>() ?: "?",
                      doc["msg"].as<const char*>() ?: "?");
    } else {
        Serial.printf("[ws] unhandled type: %s\n", t.c_str());
    }
}

void wsHandleBinary(const uint8_t* data, size_t length)
{
    if (length < 8) return;
    uint8_t ver = data[0];
    uint8_t kind = data[1];
    uint16_t sid = (uint16_t)data[2] | ((uint16_t)data[3] << 8);
    // uint32_t seq = readUint32LE(data + 4);
    if (ver != 0x01) {
        Serial.printf("[ws.bin] bad ver 0x%02x\n", ver);
        return;
    }
    if (kind == 0x02) {           // KIND_TTS_PCM
        const uint8_t* pcm = data + 8;
        size_t pcmLen = length - 8;
        if (!wsTtsActive) {
            // Auto-start a session if we missed tts.start
            wsTtsBegin(sid, 16000, 0);
        }
        if (pcmLen < 2 || wsTtsBuf == nullptr) return;
        // Grow the buffer geometrically if needed.
        if (wsTtsBufLen + pcmLen > wsTtsBufCap) {
            size_t newCap = wsTtsBufCap * 2;
            if (newCap < wsTtsBufLen + pcmLen) newCap = wsTtsBufLen + pcmLen + 4096;
            if (newCap > kWsTtsBufMaxBytes) newCap = kWsTtsBufMaxBytes;
            uint8_t* grown = (uint8_t*)heap_caps_realloc(
                wsTtsBuf, newCap, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
            if (!grown) grown = (uint8_t*)heap_caps_realloc(
                wsTtsBuf, newCap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (!grown) {
                Serial.println("[ws.tts] OOM growing tts buf — dropping chunk");
                return;
            }
            wsTtsBuf = grown;
            wsTtsBufCap = newCap;
        }
        memcpy(wsTtsBuf + wsTtsBufLen, pcm, pcmLen);
        wsTtsBufLen += pcmLen;
        wsTtsBytesPlayed += pcmLen;
        wsTtsLastChunkMs = millis();
    } else {
        Serial.printf("[ws.bin] unhandled kind=0x%02x\n", kind);
    }
}

void wsEventCallback(WStype_t type, uint8_t* payload, size_t length)
{
    switch (type) {
        case WStype_CONNECTED:
            wsConnected = true;
            wsHelloSent = false;
            Serial.printf("[ws] connected to %s:%d\n",
                          STACKPROXY_WS_HOST, (int)STACKPROXY_WS_PORT);
            wsSendHello();
            break;
        case WStype_DISCONNECTED:
            if (wsConnected) Serial.println("[ws] disconnected");
            wsConnected = false;
            wsHelloSent = false;
            break;
        case WStype_TEXT:
            wsHandleText(reinterpret_cast<const char*>(payload), length);
            break;
        case WStype_BIN:
            wsHandleBinary(payload, length);
            break;
        case WStype_ERROR:
            Serial.printf("[ws] error: %.*s\n", (int)length, payload);
            break;
        case WStype_PING:
        case WStype_PONG:
        case WStype_FRAGMENT:
        case WStype_FRAGMENT_BIN_START:
        case WStype_FRAGMENT_TEXT_START:
        case WStype_FRAGMENT_FIN:
        default:
            break;
    }
}

void wsBegin()
{
    const char* host = STACKPROXY_WS_HOST;
    if (host == nullptr || host[0] == '\0') {
        Serial.println("[ws] STACKPROXY_WS_HOST empty, ws client disabled");
        return;
    }
    if (wsOutQueue == nullptr) {
        wsOutQueue = xQueueCreate(kWsOutQueueLen, sizeof(WsOutMsg));
    }
    if (wsOutBinQueue == nullptr) {
        wsOutBinQueue = xQueueCreate(kWsOutBinQueueLen, sizeof(WsOutBin));
    }
    deviceId = wsMakeDeviceId();
    connectionId = wsMakeConnectionId();
    Serial.printf("[ws] device_id=%s boot_count=%u connection_id=%s\n",
                  deviceId.c_str(), bootCount, connectionId.c_str());
    wsClient.begin(host, STACKPROXY_WS_PORT, "/", "ocsc.v1");
    wsClient.onEvent(wsEventCallback);
    wsClient.setReconnectInterval(2000);
    wsClient.enableHeartbeat(15000, 5000, 2);
    wsBound = true;
}


// =========================================================================
// Mic streaming (PR5) — head.tap toggles ws PCM upload
// =========================================================================

constexpr uint32_t kMicStreamChunkSamples = 1600;   // 100 ms @ 16 kHz
constexpr uint32_t kMicStreamMaxSecs = 60;          // hard safety only — user stops manually
constexpr uint32_t kMicStreamSampleRate = 16000;

void micStreamTask(void* /*arg*/)
{
    int16_t* buf = static_cast<int16_t*>(
        heap_caps_malloc(kMicStreamChunkSamples * 2,
                          MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (!buf) {
        Serial.println("[mic.stream] OOM");
        vTaskDelete(nullptr);
        return;
    }
    // Stop speaker (half-duplex i2s)
    if (M5.Speaker.isPlaying()) {
        M5.Speaker.stop();
        vTaskDelay(pdMS_TO_TICKS(40));
    }
    if (!M5.Mic.isEnabled() || !M5.Mic.begin()) {
        Serial.println("[mic.stream] mic begin failed");
        pushEvent("mic.error", "stream_begin");
        free(buf);
        micStreaming = false;
        vTaskDelete(nullptr);
        return;
    }

    // Update screen UI — LED ring switches to the hard-blink red Recording
    // pattern so it's obvious at a glance that the mic is open.
    lastTitle = "正在录音 (REC)";
    lastText = "再次摸头停止";
    displayMode = DisplayMode::Message;
    displayNeedsRedraw = millis();
    faceExpression = "thinking";
    ledEffect = LedEffect::Recording;
    // Force fast LED refresh so the 200ms time-based blink animates smoothly
    ledSpeed = 50;

    micStreamSid = (uint16_t)((bootCount << 8) | (millis() & 0xFF));
    if (micStreamSid == 0) micStreamSid = 1;
    {
        StaticJsonDocument<128> doc;
        doc["t"] = "mic.start";
        doc["sid"] = micStreamSid;
        doc["sr"] = (int)kMicStreamSampleRate;
        doc["fmt"] = "pcm16";
        String payload; serializeJson(doc, payload);
        wsEnqueueText(payload);
    }
    pushEvent("mic.start", "");

    micStreamStartMs = millis();
    uint32_t seq = 0;
    const char* endReason = "timeout";
    while (!micStreamStopRequested
           && (millis() - micStreamStartMs) < kMicStreamMaxSecs * 1000) {
        if (!M5.Mic.record(buf, kMicStreamChunkSamples, kMicStreamSampleRate)) {
            Serial.println("[mic.stream] record rejected");
            break;
        }
        while (M5.Mic.isRecording() && !micStreamStopRequested) {
            vTaskDelay(pdMS_TO_TICKS(5));
        }
        size_t outLen = 0;
        uint8_t* frame = wsPackBinaryFrame(
            0x01, micStreamSid, seq, buf,
            kMicStreamChunkSamples * 2, &outLen);
        if (frame) {
            wsEnqueueBin(frame, outLen);
        }
        seq++;
    }
    if (micStreamStopRequested) endReason = "tap";

    // Release the mic and properly re-arm the speaker. M5Unified internals:
    //   Mic.end()      → i2s_driver_uninstall()  (I2S driver is GONE)
    //   Speaker.begin() alone → early-returns because _task_running is still
    //                           true → never reinstalls I2S → playRaw fails
    // The correct pair is Speaker.end() THEN Speaker.begin():
    //   Speaker.end()   → _task_running=false, kills spk_task
    //   Speaker.begin() → now runs _setup_i2s() → I2S reinstalled in TX mode
    M5.Mic.end();
    M5.Speaker.end();
    M5.Speaker.begin();
    M5.Speaker.setVolume(150);

    {
        StaticJsonDocument<128> doc;
        doc["t"] = "mic.end";
        doc["sid"] = micStreamSid;
        doc["reason"] = endReason;
        doc["total"] = (uint32_t)seq;
        String payload; serializeJson(doc, payload);
        wsEnqueueText(payload);
    }
    pushEvent("mic.done", "");

    // UI feedback: waiting for ASR / agent reply
    lastTitle = "Thinking";
    lastText = "正在处理...请稍候";
    displayMode = DisplayMode::Message;
    displayNeedsRedraw = millis();
    faceExpression = "thinking";
    ledEffect = LedEffect::Thinking;
    // Arm conversation watchdog so we don't hang if ASR/agent never reply
    convoWatchdogDeadlineMs = millis() + kConvoWatchdogMs;

    micStreaming = false;
    micStreamStopRequested = false;
    free(buf);
    micStreamTaskHandle = nullptr;
    vTaskDelete(nullptr);
}

// Reset UI back to idle (called on tts.done, agent.error, asr.final-empty,
// or watchdog timeout). Safe to call from main loop or ws callback.
void convoResetIdle(const char* title = "StackChan",
                     const char* text  = "Ready")
{
    convoWatchdogDeadlineMs = 0;
    faceExpression = "neutral";
    ledEffect = LedEffect::Solid;
    lastTitle = title;
    lastText = text;
    displayMode = DisplayMode::Message;
    displayNeedsRedraw = millis();
}

// (Beep removed — see convoBeep no-op below. The class-D amp on this board
// pops audibly on any short burst, so feedback is LED-only now.)

// Beep disabled — class-D amp pops on every short burst regardless of
// envelope. Feedback is now LED-only (LedEffect::Recording).
void convoBeep(uint16_t /*freq*/, uint32_t /*toneMs*/) { /* no-op */ }

// (Stop button overlay removed — feedback is LED-only now per UX request.
// Recording state is shown via LedEffect::Recording red blink + screen
// title "正在录音 (REC)".)
bool shouldShowStopButton() { return false; }
void drawStopButtonOverlay() {}

void micStreamStart()
{
    if (micStreaming) return;
    if (!wsClient.isConnected()) {
        Serial.println("[mic.stream] ws not connected, refusing start");
        return;
    }
    micStreaming = true;
    micStreamStopRequested = false;
    xTaskCreatePinnedToCore(micStreamTask, "micStream", 6144, nullptr, 1,
                             &micStreamTaskHandle, 0);
}

void micStreamStop()
{
    if (!micStreaming) return;
    micStreamStopRequested = true;
}

void micStreamToggle()
{
    if (micStreaming) micStreamStop();
    else micStreamStart();
}

// Drain outbound queue (call from main loop).
void wsDrainOutbound()
{
    if (!wsClient.isConnected()) {
        // Connection down — drop any queued binary buffers to avoid heap growth
        if (wsOutBinQueue) {
            WsOutBin drop;
            while (xQueueReceive(wsOutBinQueue, &drop, 0) == pdTRUE) {
                free(drop.data);
            }
        }
        return;
    }
    if (wsOutQueue) {
        WsOutMsg msg;
        int budget = 8;
        while (budget-- > 0 && xQueueReceive(wsOutQueue, &msg, 0) == pdTRUE) {
            wsClient.sendTXT(msg.buf);
        }
    }
    if (wsOutBinQueue) {
        WsOutBin item;
        int budget = 4;     // limit per-loop binary throughput
        while (budget-- > 0 && xQueueReceive(wsOutBinQueue, &item, 0) == pdTRUE) {
            wsClient.sendBIN(item.data, item.length);
            free(item.data);
        }
    }
}

void wsLoop()
{
    if (!wsBound) return;
    wsClient.loop();
    wsDrainOutbound();
}

void setupRoutes()
{
    server.on("/", HTTP_GET, handleRoot);
    server.on("/api", HTTP_GET, handleApiIndex);
    server.on("/status", HTTP_GET, handleStatus);
    server.on("/battery", HTTP_GET, handleBattery);
    server.on("/sensors", HTTP_GET, handleSensors);
    server.on("/events", HTTP_GET, handleEvents);

    server.on("/display", HTTP_POST, handleDisplay);
    server.on("/display/clear", HTTP_POST, handleDisplayClear);
    server.on("/display/face", HTTP_POST, handleDisplayFace);
    server.on("/display/big", HTTP_POST, handleDisplayBig);
    server.on("/display/progress", HTTP_POST, handleDisplayProgress);
    server.on("/display/marquee", HTTP_POST, handleDisplayMarquee);
    server.on("/display/brightness", HTTP_POST, handleDisplayBrightness);
    server.on("/display/status", HTTP_POST, handleDisplayStatus);

    server.on("/led", HTTP_POST, handleLed);
    server.on("/led/effect", HTTP_POST, handleLedEffect);
    server.on("/led/off", HTTP_POST, handleLedOff);

    server.on("/head", HTTP_POST, handleHead);
    server.on("/head/normalized", HTTP_POST, handleHeadNormalized);
    server.on("/head/rotate", HTTP_POST, handleHeadRotate);
    server.on("/head/stop", HTTP_POST, handleHeadStop);
    server.on("/head/torque", HTTP_POST, handleHeadTorque);
    server.on("/head/calibrate", HTTP_POST, handleHeadCalibrate);
    server.on("/home", HTTP_POST, handleHome);
    server.on("/action", HTTP_POST, handleAction);

    server.on("/idle", HTTP_POST, handleIdle);
    server.on("/breathing", HTTP_POST, handleBreathing);

    server.on("/speak", HTTP_POST, handleSpeak);
    server.on("/speak/status", HTTP_GET, handleSpeakStatus);
    server.on("/speak/stop", HTTP_POST, handleSpeakStop);

    server.on("/mic/record", HTTP_POST, handleMicRecord);
    server.on("/mic/status", HTTP_GET, handleMicStatus);
    server.on("/mic/stop", HTTP_POST, handleMicStop);
    server.on("/mic/last", HTTP_GET, handleMicLast);

    server.on("/camera/init", HTTP_POST, handleCameraInit);
    server.on("/camera/deinit", HTTP_POST, handleCameraDeinit);
    server.on("/camera/status", HTTP_GET, handleCameraStatus);
    server.on("/camera/capture", HTTP_GET, handleCameraCapture);
    server.on("/camera/capture.jpg", HTTP_GET, handleCameraCapture);
    server.on("/camera/config", HTTP_POST, handleCameraConfig);
    server.on("/camera/preview", HTTP_POST, handleCameraPreview);

    server.on("/webhook", HTTP_POST, handleWebhookSet);
    server.on("/webhook", HTTP_GET, handleWebhookGet);

    server.on("/volume", HTTP_POST, handleVolume);
    server.on("/reboot", HTTP_POST, handleReboot);

    server.onNotFound([]() {
        if (server.method() == HTTP_OPTIONS) { handleOptions(); return; }
        sendError(404, "not found");
    });

    // Collect headers we care about
    static const char* kHeaders[] = { "X-StackChan-Token" };
    server.collectHeaders(kHeaders, 1);
}

}  // namespace

// =========================================================================
// Setup + loop
// =========================================================================

void setup()
{
    Serial.begin(115200);
    delay(200);

    M5StackChan.begin();
    M5StackChan.Motion.goHome();
    setAllLeds(0, 24, 48);

    {
        // Original /speak-proven DMA config — known good for buffered
        // playRaw. (Earlier experiments with 16 buffers / priority 5 were
        // reverted; they didn't help and complicated the mic↔speaker
        // hand-off.)
        auto spkCfg = M5.Speaker.config();
        spkCfg.dma_buf_count = 8;
        spkCfg.dma_buf_len = 1024;
        spkCfg.task_priority = 2;
        M5.Speaker.config(spkCfg);
    }
    M5.Speaker.begin();
    M5.Speaker.setVolume(150);

    eventMutex = xSemaphoreCreateMutex();
    cameraMutex = xSemaphoreCreateMutex();
    cameraJobDone = xSemaphoreCreateBinary();
    // Pinned to core 1 (the app/loop core), NOT core 0: the WiFi + lwIP stack
    // lives on core 0, and the heavy esp_camera_init / frame2jpg work starves it
    // — the device's WiFi dies and does not recover. Core 1 keeps camera work
    // away from the radio (this is where the old in-loop() camera code ran).
    xTaskCreatePinnedToCore(cameraTask, "cameraTask", 8192, nullptr, 1, &cameraTaskHandle, 1);

    speakStartTask();
    micStartTask();
    webhookStartTask();

    wsLoadOrBumpBootCount();
    Serial.printf("[boot] boot_count=%u fw=%s\n", bootCount, kFirmwareVersion);

    connectNetwork();
    setupRoutes();
    server.begin();

    // Camera is initialised on first /camera/init or /camera/capture request,
    // not at boot. That keeps the device responsive for users who never need
    // vision, and avoids the long PSRAM allocation slowing WiFi handshake.

    drawStatusScreen();
    Serial.println("StackChan OpenClaw HTTP API ready");
    if (WiFi.isConnected()) {
        wsBegin();
    } else {
        Serial.println("[ws] WiFi not connected, ws disabled");
    }
    pushEvent("boot", kFirmwareVersion);
}

void loop()
{
    M5StackChan.update();
    server.handleClient();
    wsLoop();

    ledLoopUpdate();
    sensorPoll();
    idleMotionTick();
    breathingTick();
    faceAnimationTick();

    // Conversation watchdog: if mic.end fired but no tts.start arrived
    // within kConvoWatchdogMs, recover the UI so user isn't stuck.
    if (convoWatchdogDeadlineMs != 0 && millis() > convoWatchdogDeadlineMs
        && !wsTtsActive && !micStreaming) {
        Serial.println("[convo] watchdog fired — resetting idle");
        convoResetIdle("Timed out", "Tap to retry");
    }

    // Trigger redraw if a worker task (mic stream, ws asr partial, etc.)
    // bumped displayNeedsRedraw. We compare with lastDisplayDrawMs so each
    // bump fires exactly one redraw.
    static uint32_t lastDisplayDrawMs = 0;
    static bool stopBtnVisible = false;
    if (displayNeedsRedraw != 0 && displayNeedsRedraw != lastDisplayDrawMs) {
        lastDisplayDrawMs = displayNeedsRedraw;
        if (displayMode == DisplayMode::Message) {
            drawMessage();
        } else if (displayMode == DisplayMode::Status) {
            drawStatusScreen();
        }
        // After any redraw, repaint the STOP overlay if we're recording.
        if (shouldShowStopButton()) {
            drawStopButtonOverlay();
            stopBtnVisible = true;
        } else {
            stopBtnVisible = false;
        }
    } else if (shouldShowStopButton() && !stopBtnVisible) {
        // STOP just appeared — make sure it's on screen
        drawStopButtonOverlay();
        stopBtnVisible = true;
    } else if (!shouldShowStopButton() && stopBtnVisible) {
        // STOP just disappeared — force a clean redraw to erase it
        if (displayMode == DisplayMode::Message) drawMessage();
        else if (displayMode == DisplayMode::Status) drawStatusScreen();
        stopBtnVisible = false;
    }

    if (displayMode == DisplayMode::Message && totalPages > 1
        && millis() - lastPageMillis >= kPageIntervalMs) {
        currentPage = (currentPage + 1) % totalPages;
        renderMessagePage();
        lastPageMillis = millis();
    }

    if (displayMode == DisplayMode::Marquee && millis() - lastMarqueeMs >= marqueeStepMs) {
        lastMarqueeMs = millis();
        renderMarqueeFrame();
    }

    delay(2);
}

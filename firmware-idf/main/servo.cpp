// Feetech SCS-bus servo driver — see servo.h.

#include "servo.h"

#include <string.h>

#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <math.h>

static const char *TAG = "servo";

namespace {

constexpr uart_port_t UART_PORT = UART_NUM_1;
constexpr gpio_num_t  PIN_TX    = GPIO_NUM_6;
constexpr gpio_num_t  PIN_RX    = GPIO_NUM_7;
constexpr int         BAUD      = 1000000;

constexpr uint8_t  INST_WRITE         = 0x03;
constexpr uint8_t  ADDR_TORQUE_ENABLE = 0x28;   // SCSCL_TORQUE_ENABLE
constexpr uint8_t  ADDR_GOAL_POSITION = 0x2A;   // SCSCL_GOAL_POSITION_L

// Yaw raw range  ≈ [200, 824] → physical [-90°, +90°].
// Pitch raw range ≈ [200, 824] → physical [head down, head up].
// 512 is the mechanical centre for both axes.
//
// StackChan-unit → raw mapping (compatible with the old firmware):
//   1 stackchan unit (10/° spec) = 0.3125 raw  (factor 16/50 in fixed-point).
constexpr int RAW_CENTER       = 512;
constexpr int Y_HOME_SC_UNITS  = 425;   // matches old fw's home pitch.

constexpr int SPEED_DEFAULT    = 200;

bool g_ready = false;

// Motion state — tracked by servo_move() so the idle / breathing
// controllers can modulate around the most recent commanded pose
// without snapping back to home each tick.
int           g_cur_x        = 0;
int           g_cur_y        = Y_HOME_SC_UNITS;
volatile bool g_idle_on      = false;
volatile bool g_breath_on    = false;
TaskHandle_t  g_motion_task  = nullptr;

constexpr uint32_t IDLE_MIN_INTERVAL_MS = 4000;
constexpr uint32_t IDLE_MAX_INTERVAL_MS = 9000;
constexpr int      IDLE_YAW_RANGE       = 450;   // ±450 sc-units
constexpr int      IDLE_PITCH_LOW       = 300;
constexpr int      IDLE_PITCH_HIGH      = 600;
constexpr int      BREATH_PITCH_AMP     = 20;    // ±20 sc-units
constexpr uint32_t BREATH_TICK_MS       = 60;

uint8_t checksum(const uint8_t *buf, size_t len) {
    uint32_t sum = 0;
    for (size_t i = 0; i < len; ++i) sum += buf[i];
    return (uint8_t)(~(sum & 0xFF));
}

int clamp_int(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

uint16_t yaw_to_raw(int x) {
    // x in [-1280, 1280] (stackchan units); 0 → RAW_CENTER.
    // 1 sc-unit = 0.3125 raw; clamp output to safe 200..824 band.
    int raw = RAW_CENTER + (x * 16) / 50;
    return (uint16_t)clamp_int(raw, 200, 824);
}

uint16_t pitch_to_raw(int y) {
    // y in [0, 850]; y == Y_HOME_SC_UNITS (425) → RAW_CENTER.
    // Direction sign: a higher y means "head looking down" in the old
    // firmware. Flip the sign so larger y still moves the head down
    // physically (assuming the servo's positive direction is "up";
    // calibrate by inspection on the bench).
    int raw = RAW_CENTER - ((y - Y_HOME_SC_UNITS) * 16) / 50;
    return (uint16_t)clamp_int(raw, 200, 824);
}

esp_err_t send_packet(uint8_t id, uint8_t addr,
                      const uint8_t *data, uint8_t n) {
    // Packet layout: FF FF ID LEN INST ADDR DATA[N] CKSUM
    // LEN = n + 3 (inst + addr + checksum-bytes-not-counted-here is 2
    //              for the "msg payload" sum: inst + addr + N, but the
    //              wire LEN per Feetech spec is N + 2 (when no addr) or
    //              N + 3 (with addr). We always include addr.)
    uint8_t buf[16];
    if (n + 7 > sizeof(buf)) return ESP_ERR_INVALID_ARG;
    buf[0] = 0xFF;
    buf[1] = 0xFF;
    buf[2] = id;
    buf[3] = (uint8_t)(n + 3);
    buf[4] = INST_WRITE;
    buf[5] = addr;
    if (data && n) memcpy(buf + 6, data, n);
    buf[6 + n] = checksum(buf + 2, 4 + n);    // ID + LEN + INST + ADDR + data
    int written = uart_write_bytes(UART_PORT, (const char *)buf, 7 + n);
    if (written < 0) return ESP_FAIL;
    // Wait for bytes to clear so multiple back-to-back commands don't
    // collide on the half-duplex bus. At 1 Mbps a 12-byte packet is
    // ~120 µs; 1 ms is generous.
    uart_wait_tx_done(UART_PORT, pdMS_TO_TICKS(2));
    return ESP_OK;
}

void host_to_scs_be(uint16_t v, uint8_t *hi, uint8_t *lo) {
    // M5's _scs_bus uses End=1 (big-endian on the wire).
    *hi = (uint8_t)(v >> 8);
    *lo = (uint8_t)(v & 0xFF);
}

}  // namespace

esp_err_t servo_init(void) {
    if (g_ready) return ESP_OK;
    uart_config_t cfg = {};
    cfg.baud_rate           = BAUD;
    cfg.data_bits           = UART_DATA_8_BITS;
    cfg.parity              = UART_PARITY_DISABLE;
    cfg.stop_bits           = UART_STOP_BITS_1;
    cfg.flow_ctrl           = UART_HW_FLOWCTRL_DISABLE;
    cfg.source_clk          = UART_SCLK_DEFAULT;
    cfg.rx_flow_ctrl_thresh = 0;
    esp_err_t err = uart_driver_install(UART_PORT, 256, 256, 0, nullptr, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_driver_install: %s", esp_err_to_name(err));
        return err;
    }
    err = uart_param_config(UART_PORT, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_param_config: %s", esp_err_to_name(err));
        return err;
    }
    err = uart_set_pin(UART_PORT, PIN_TX, PIN_RX, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_set_pin: %s", esp_err_to_name(err));
        return err;
    }
    g_ready = true;
    ESP_LOGI(TAG, "SCS bus ready: UART%d @ %d, TX=%d RX=%d",
             (int)UART_PORT, BAUD, (int)PIN_TX, (int)PIN_RX);
    return ESP_OK;
}

esp_err_t servo_write_pos(uint8_t id, uint16_t pos,
                          uint16_t time_ms, uint16_t speed) {
    if (!g_ready) return ESP_ERR_INVALID_STATE;
    uint8_t d[6];
    host_to_scs_be(pos,     &d[0], &d[1]);
    host_to_scs_be(time_ms, &d[2], &d[3]);
    host_to_scs_be(speed,   &d[4], &d[5]);
    return send_packet(id, ADDR_GOAL_POSITION, d, sizeof(d));
}

esp_err_t servo_torque(uint8_t id, bool enabled) {
    if (!g_ready) return ESP_ERR_INVALID_STATE;
    uint8_t v = enabled ? 1 : 0;
    return send_packet(id, ADDR_TORQUE_ENABLE, &v, 1);
}

void servo_move(int x, int y, int speed) {
    if (!g_ready) return;
    int s = clamp_int(speed, 0, 1023);
    uint16_t yaw_raw   = yaw_to_raw(x);
    uint16_t pitch_raw = pitch_to_raw(y);
    servo_write_pos(1, yaw_raw,   0, (uint16_t)s);
    servo_write_pos(2, pitch_raw, 0, (uint16_t)s);
    g_cur_x = clamp_int(x, -1280, 1280);
    g_cur_y = clamp_int(y,     0,  850);
}

void servo_home(void) {
    servo_move(0, Y_HOME_SC_UNITS, SPEED_DEFAULT);
}

bool servo_is_ready(void) { return g_ready; }

void servo_get_pos(int *x, int *y) {
    if (x) *x = g_cur_x;
    if (y) *y = g_cur_y;
}

namespace {

// Motion controller task: ticks every BREATH_TICK_MS, runs both the
// breathing oscillation and the idle "look around" scheduler off the
// same clock. Keeping them in one task avoids servo bus contention
// (each write is ~120 µs on the wire but we still want to serialize).
void motion_task(void *arg) {
    (void)arg;
    uint32_t next_idle_ms = 0;
    uint32_t breath_phase = 0;     // 0..99
    int      breath_baseY = -1;

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(BREATH_TICK_MS));

        const uint32_t now = (uint32_t)(esp_log_timestamp());

        // ── Idle "look around" ────────────────────────────────────────
        if (g_idle_on) {
            if (next_idle_ms == 0) {
                next_idle_ms = now + IDLE_MIN_INTERVAL_MS +
                               (esp_random() % (IDLE_MAX_INTERVAL_MS - IDLE_MIN_INTERVAL_MS));
            } else if ((int32_t)(now - next_idle_ms) >= 0) {
                next_idle_ms = now + IDLE_MIN_INTERVAL_MS +
                               (esp_random() % (IDLE_MAX_INTERVAL_MS - IDLE_MIN_INTERVAL_MS));
                int dx = (int)(esp_random() % (IDLE_YAW_RANGE * 2 + 1)) - IDLE_YAW_RANGE;
                int dy = IDLE_PITCH_LOW +
                         (int)(esp_random() % (IDLE_PITCH_HIGH - IDLE_PITCH_LOW + 1));
                servo_move(dx, dy, 350);
                // Reset breathing base to track the new pose.
                breath_baseY = g_cur_y;
            }
        } else {
            next_idle_ms = 0;
        }

        // ── Breathing pitch oscillation ───────────────────────────────
        if (g_breath_on) {
            if (breath_baseY < 0) breath_baseY = g_cur_y;
            breath_phase = (breath_phase + 1) % 100;
            float  t  = (breath_phase / 100.0f) * 6.2831853f;
            int    dy = (int)(sinf(t) * BREATH_PITCH_AMP);
            int    target_y = clamp_int(breath_baseY + dy, 0, 850);
            // Direct write_pos so we don't overwrite g_cur_y — the
            // user's commanded baseline stays put, only the wire output
            // wobbles.
            uint16_t pitch_raw = pitch_to_raw(target_y);
            servo_write_pos(2, pitch_raw, 0, (uint16_t)200);
        } else {
            breath_baseY = -1;
        }
    }
}

void ensure_motion_task() {
    if (g_motion_task) return;
    xTaskCreatePinnedToCore(motion_task, "servo_motion", 3072, nullptr,
                            tskIDLE_PRIORITY + 1, &g_motion_task, 0);
}

}  // namespace

void servo_set_idle(bool enabled) {
    g_idle_on = enabled;
    if (enabled) ensure_motion_task();
}
bool servo_get_idle(void) { return g_idle_on; }

void servo_set_breathing(bool enabled) {
    g_breath_on = enabled;
    if (enabled) ensure_motion_task();
}
bool servo_get_breathing(void) { return g_breath_on; }

void servo_stop_motion(void) {
    g_idle_on = false;
    g_breath_on = false;
    if (!g_ready) return;
    // Re-issue current commanded pose at default speed — interrupts
    // any in-flight tween and ensures bus reaches a known state.
    uint16_t yaw_raw   = yaw_to_raw(g_cur_x);
    uint16_t pitch_raw = pitch_to_raw(g_cur_y);
    servo_write_pos(1, yaw_raw,   0, SPEED_DEFAULT);
    servo_write_pos(2, pitch_raw, 0, SPEED_DEFAULT);
}

// ── Gesture sequences ────────────────────────────────────────────────────
// All movements are in StackChan units (10 = 1°). Speed 200 ≈ natural
// turn; 400-600 for fast / startled; 100 for slow / sleepy.

namespace {

void wait_ms(int ms) { vTaskDelay(pdMS_TO_TICKS(ms)); }

// Look around: sweep yaw left → right → centre.
void action_look_around() {
    servo_move(-800, Y_HOME_SC_UNITS, 250); wait_ms(900);
    servo_move( 800, Y_HOME_SC_UNITS, 250); wait_ms(1400);
    servo_move(   0, Y_HOME_SC_UNITS, 250); wait_ms(700);
}

void action_nod() {
    servo_move(0, Y_HOME_SC_UNITS - 250, 350); wait_ms(450);
    servo_move(0, Y_HOME_SC_UNITS + 150, 350); wait_ms(450);
    servo_move(0, Y_HOME_SC_UNITS,       350); wait_ms(300);
}

void action_shake() {
    servo_move(-450, Y_HOME_SC_UNITS, 400); wait_ms(350);
    servo_move( 450, Y_HOME_SC_UNITS, 400); wait_ms(350);
    servo_move(-300, Y_HOME_SC_UNITS, 400); wait_ms(300);
    servo_move(   0, Y_HOME_SC_UNITS, 400); wait_ms(300);
}

void action_yes() {
    action_nod();
    wait_ms(150);
    action_nod();
}

void action_no() {
    action_shake();
    wait_ms(150);
    action_shake();
}

void action_dance() {
    for (int i = 0; i < 3; ++i) {
        servo_move(-600, Y_HOME_SC_UNITS - 150, 500); wait_ms(350);
        servo_move( 600, Y_HOME_SC_UNITS + 150, 500); wait_ms(350);
    }
    servo_move(0, Y_HOME_SC_UNITS, 300); wait_ms(400);
}

void action_surprised() {
    // Jerk back + look up sharply, then settle.
    servo_move(0, Y_HOME_SC_UNITS - 350, 600); wait_ms(400);
    servo_move(0, Y_HOME_SC_UNITS - 100, 250); wait_ms(700);
}

void action_panic() {
    for (int i = 0; i < 4; ++i) {
        servo_move(-500, Y_HOME_SC_UNITS - 100, 600); wait_ms(200);
        servo_move( 500, Y_HOME_SC_UNITS + 100, 600); wait_ms(200);
    }
    servo_move(0, Y_HOME_SC_UNITS, 250); wait_ms(400);
}

void action_bow() {
    // Slow head dip forward, hold, recover.
    servo_move(0, Y_HOME_SC_UNITS + 380, 250); wait_ms(900);
    servo_move(0, Y_HOME_SC_UNITS,       200); wait_ms(500);
}

void action_peek() {
    // Tilt + lean forward as if peeking.
    servo_move(280, Y_HOME_SC_UNITS - 200, 250); wait_ms(900);
    servo_move(  0, Y_HOME_SC_UNITS,       250); wait_ms(400);
}

void action_tilt(bool left) {
    int x = left ? -300 : 300;
    servo_move(x, Y_HOME_SC_UNITS, 200); wait_ms(700);
    servo_move(0, Y_HOME_SC_UNITS, 200); wait_ms(300);
}

void action_sleep() {
    servo_move(0, Y_HOME_SC_UNITS + 350, 150); wait_ms(1100);
    servo_torque(1, false);
    servo_torque(2, false);
}

void action_wake() {
    servo_torque(1, true);
    servo_torque(2, true);
    wait_ms(80);
    servo_move(0, Y_HOME_SC_UNITS - 100, 250); wait_ms(500);
    servo_move(0, Y_HOME_SC_UNITS,       250); wait_ms(400);
}

}  // namespace

esp_err_t servo_action(const char *name) {
    if (!g_ready || !name) return ESP_ERR_INVALID_STATE;
    if      (strcmp(name, "home")        == 0) { servo_home();           wait_ms(400); }
    else if (strcmp(name, "nod")         == 0) action_nod();
    else if (strcmp(name, "shake")       == 0) action_shake();
    else if (strcmp(name, "yes")         == 0) action_yes();
    else if (strcmp(name, "no")          == 0) action_no();
    else if (strcmp(name, "look_up")     == 0) { servo_move(0, Y_HOME_SC_UNITS - 350, 250); wait_ms(700); }
    else if (strcmp(name, "look_down")   == 0) { servo_move(0, Y_HOME_SC_UNITS + 350, 250); wait_ms(700); }
    else if (strcmp(name, "look_left")   == 0) { servo_move(-800, Y_HOME_SC_UNITS, 250); wait_ms(700); }
    else if (strcmp(name, "look_right")  == 0) { servo_move( 800, Y_HOME_SC_UNITS, 250); wait_ms(700); }
    else if (strcmp(name, "look_around") == 0) action_look_around();
    else if (strcmp(name, "dance")       == 0) action_dance();
    else if (strcmp(name, "surprised")   == 0) action_surprised();
    else if (strcmp(name, "sleep")       == 0) action_sleep();
    else if (strcmp(name, "wake")        == 0) action_wake();
    else if (strcmp(name, "panic")       == 0) action_panic();
    else if (strcmp(name, "peek")        == 0) action_peek();
    else if (strcmp(name, "bow")         == 0) action_bow();
    else if (strcmp(name, "tilt_left")   == 0) action_tilt(true);
    else if (strcmp(name, "tilt_right")  == 0) action_tilt(false);
    else return ESP_ERR_NOT_FOUND;
    return ESP_OK;
}

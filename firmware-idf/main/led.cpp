// LED ring — see led.h. Mirrors the protocol from
// Arduino-side `M5StackChan/src/drivers/PY32IOExpander/PY32IOExpander.cpp`
// trimmed to just the LED RAM region we need.

#include "led.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "led";

namespace {

constexpr uint8_t  PY32_ADDR         = 0x6F;
constexpr uint32_t I2C_HZ            = 100000;       // PY32 datasheet caps fast-mode
constexpr int      I2C_TIMEOUT_MS    = 50;
constexpr uint8_t  LED_COUNT         = 12;

// Register map (subset)
constexpr uint8_t  REG_VERSION       = 0x02;
constexpr uint8_t  REG_GPIO_M_L      = 0x03;     // GPIO direction (bit per pin, 1=output)
constexpr uint8_t  REG_GPIO_O_L      = 0x05;     // GPIO output state (bit per pin)
constexpr uint8_t  REG_LED_CFG       = 0x24;
constexpr uint8_t  REG_LED_RAM_START = 0x30;
constexpr uint8_t  LED_REFRESH_BIT   = (1 << 6);

i2c_master_dev_handle_t g_dev = nullptr;

// ── Effect state (driven by tick_task) ──────────────────────────────────
enum class Effect : uint8_t {
    Off, Solid, Rainbow, Breathing, Pulse, Scanner, Wipe, Sparkle,
    Police, Fire, Chase, Theater, Listening, Thinking, Talking, Recording,
};

struct EffectState {
    Effect   effect;
    uint8_t  r, g, b;
    uint16_t speed_ms;
    uint32_t frame;   // monotonic frame counter
};

// Updated by led_set_effect (any task) and consumed by tick_task.
// Reads/writes are 32-bit aligned on Xtensa so torn reads are unlikely;
// guard with a spinlock anyway to keep behaviour deterministic.
portMUX_TYPE   g_eff_lock = portMUX_INITIALIZER_UNLOCKED;
EffectState    g_eff      = { Effect::Solid, 0, 0, 0, 0, 0 };
bool           g_eff_dirty = false;   // colour/effect change → next tick repaints
TaskHandle_t   g_tick_task = nullptr;

esp_err_t write_reg(uint8_t reg, uint8_t value) {
    uint8_t buf[2] = { reg, value };
    return i2c_master_transmit(g_dev, buf, sizeof(buf), I2C_TIMEOUT_MS);
}

esp_err_t write_regs(uint8_t reg, const uint8_t *data, size_t len) {
    if (len == 0 || len > 64) return ESP_ERR_INVALID_ARG;
    uint8_t buf[1 + 64];
    buf[0] = reg;
    memcpy(buf + 1, data, len);
    return i2c_master_transmit(g_dev, buf, len + 1, I2C_TIMEOUT_MS);
}

esp_err_t read_reg(uint8_t reg, uint8_t *out) {
    return i2c_master_transmit_receive(g_dev, &reg, 1, out, 1, I2C_TIMEOUT_MS);
}

esp_err_t refresh() {
    uint8_t cfg = 0;
    esp_err_t err = read_reg(REG_LED_CFG, &cfg);
    if (err != ESP_OK) return err;
    return write_reg(REG_LED_CFG, cfg | LED_REFRESH_BIT);
}

uint16_t rgb_to_565_le(uint8_t r, uint8_t g, uint8_t b) {
    uint16_t v = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
    // Chip wants little-endian on the wire: low byte first at offset 0
    return v;  // we'll pack as {LSB,MSB} below
}

// Set per-LED color without triggering refresh, so the whole frame
// lands as one atomic latch via refresh() at the end.
esp_err_t set_pixel_no_refresh(uint8_t index, uint8_t r, uint8_t g, uint8_t b) {
    uint16_t c = rgb_to_565_le(r, g, b);
    uint8_t  data[2] = { (uint8_t)(c & 0xFF), (uint8_t)((c >> 8) & 0xFF) };
    return write_regs((uint8_t)(REG_LED_RAM_START + index * 2), data, 2);
}

// HSV → RGB. h in [0,360), s/v in [0,1]. Used by Rainbow.
void hsv_to_rgb(float h, float s, float v, uint8_t &r, uint8_t &g, uint8_t &b) {
    float c = v * s;
    float hp = h / 60.0f;
    float x = c * (1.0f - fabsf(fmodf(hp, 2.0f) - 1.0f));
    float rp = 0, gp = 0, bp = 0;
    if      (hp < 1) { rp = c; gp = x; }
    else if (hp < 2) { rp = x; gp = c; }
    else if (hp < 3) { gp = c; bp = x; }
    else if (hp < 4) { gp = x; bp = c; }
    else if (hp < 5) { rp = x; bp = c; }
    else             { rp = c; bp = x; }
    float m = v - c;
    r = (uint8_t)((rp + m) * 255.0f);
    g = (uint8_t)((gp + m) * 255.0f);
    b = (uint8_t)((bp + m) * 255.0f);
}

void render_frame(const EffectState &s) {
    const uint8_t R = s.r, G = s.g, B = s.b;
    const uint32_t f = s.frame;
    switch (s.effect) {
    case Effect::Off:
        for (int i = 0; i < LED_COUNT; ++i) set_pixel_no_refresh(i, 0, 0, 0);
        break;
    case Effect::Solid:
        for (int i = 0; i < LED_COUNT; ++i) set_pixel_no_refresh(i, R, G, B);
        break;
    case Effect::Rainbow: {
        for (int i = 0; i < LED_COUNT; ++i) {
            float h = fmodf((float)f * 6.0f + (float)i * (360.0f / LED_COUNT), 360.0f);
            uint8_t rr, gg, bb;
            hsv_to_rgb(h, 1.0f, 1.0f, rr, gg, bb);
            set_pixel_no_refresh(i, rr, gg, bb);
        }
        break;
    }
    case Effect::Breathing: {
        float t   = fmodf((float)f * 0.05f, 6.28318f);
        float amp = (sinf(t) + 1.0f) * 0.5f;
        uint8_t rr = (uint8_t)(R * amp);
        uint8_t gg = (uint8_t)(G * amp);
        uint8_t bb = (uint8_t)(B * amp);
        for (int i = 0; i < LED_COUNT; ++i) set_pixel_no_refresh(i, rr, gg, bb);
        break;
    }
    case Effect::Pulse: {
        float t   = fmodf((float)f * 0.15f, 2.0f);
        float amp = (t < 1.0f) ? t : (2.0f - t);
        uint8_t rr = (uint8_t)(R * amp);
        uint8_t gg = (uint8_t)(G * amp);
        uint8_t bb = (uint8_t)(B * amp);
        for (int i = 0; i < LED_COUNT; ++i) set_pixel_no_refresh(i, rr, gg, bb);
        break;
    }
    case Effect::Scanner: {
        int pos = f % (2 * LED_COUNT - 2);
        int idx = (pos < LED_COUNT) ? pos : (2 * LED_COUNT - 2 - pos);
        for (int i = 0; i < LED_COUNT; ++i) {
            int d = abs(i - idx);
            if (d == 0)      set_pixel_no_refresh(i, R, G, B);
            else if (d == 1) set_pixel_no_refresh(i, R / 4, G / 4, B / 4);
            else             set_pixel_no_refresh(i, 0, 0, 0);
        }
        break;
    }
    case Effect::Wipe: {
        int pos = f % (LED_COUNT + 4);
        for (int i = 0; i < LED_COUNT; ++i) {
            if (i <= pos && pos < LED_COUNT) set_pixel_no_refresh(i, R, G, B);
            else                              set_pixel_no_refresh(i, 0, 0, 0);
        }
        break;
    }
    case Effect::Sparkle:
        for (int i = 0; i < LED_COUNT; ++i) {
            if ((esp_random() & 0x1F) == 0) set_pixel_no_refresh(i, R, G, B);
            else                             set_pixel_no_refresh(i, R / 16, G / 16, B / 16);
        }
        break;
    case Effect::Police: {
        bool red = ((f / 4) & 1) == 0;
        for (int i = 0; i < LED_COUNT; ++i) {
            bool left = i < LED_COUNT / 2;
            if (red == left) set_pixel_no_refresh(i, 255, 0, 0);
            else             set_pixel_no_refresh(i, 0,   0, 255);
        }
        break;
    }
    case Effect::Fire:
        for (int i = 0; i < LED_COUNT; ++i) {
            uint8_t flick = esp_random() & 0x7F;
            uint8_t rr    = (uint8_t)((200 + flick / 2) > 255 ? 255 : (200 + flick / 2));
            uint8_t gg    = (uint8_t)(40 + (flick / 2));
            set_pixel_no_refresh(i, rr, gg, 0);
        }
        break;
    case Effect::Chase: {
        int pos = f % LED_COUNT;
        for (int i = 0; i < LED_COUNT; ++i) {
            int d = (i - pos + LED_COUNT) % LED_COUNT;
            if      (d == 0) set_pixel_no_refresh(i, R, G, B);
            else if (d == 1) set_pixel_no_refresh(i, R / 3, G / 3, B / 3);
            else if (d == 2) set_pixel_no_refresh(i, R / 8, G / 8, B / 8);
            else             set_pixel_no_refresh(i, 0, 0, 0);
        }
        break;
    }
    case Effect::Theater: {
        int step = f % 3;
        for (int i = 0; i < LED_COUNT; ++i) {
            if (i % 3 == step) set_pixel_no_refresh(i, R, G, B);
            else                set_pixel_no_refresh(i, 0, 0, 0);
        }
        break;
    }
    case Effect::Listening: {
        float t   = fmodf((float)f * 0.08f, 6.28318f);
        float amp = 0.3f + (sinf(t) + 1.0f) * 0.35f;
        for (int i = 0; i < LED_COUNT; ++i) {
            set_pixel_no_refresh(i, 0, (uint8_t)(80 * amp), (uint8_t)(200 * amp));
        }
        break;
    }
    case Effect::Thinking: {
        int pos = f % LED_COUNT;
        for (int i = 0; i < LED_COUNT; ++i) {
            int d = (i - pos + LED_COUNT) % LED_COUNT;
            if (d == 0) set_pixel_no_refresh(i, 180, 80, 220);
            else        set_pixel_no_refresh(i,  20,  6,  30);
        }
        break;
    }
    case Effect::Talking: {
        float t   = fmodf((float)f * 0.25f, 6.28318f);
        float amp = 0.4f + (sinf(t) + 1.0f) * 0.3f;
        for (int i = 0; i < LED_COUNT; ++i) {
            set_pixel_no_refresh(i, 0, (uint8_t)(220 * amp), (uint8_t)(60 * amp));
        }
        break;
    }
    case Effect::Recording: {
        bool on = ((esp_timer_get_time() / 200000) & 1) != 0;
        for (int i = 0; i < LED_COUNT; ++i) {
            if (on) set_pixel_no_refresh(i, 255, 0, 0);
            else    set_pixel_no_refresh(i, 0,   0, 0);
        }
        break;
    }
    }
    refresh();
}

bool effect_animates(Effect e) {
    return e != Effect::Off && e != Effect::Solid;
}

uint16_t default_speed_ms(Effect e) {
    switch (e) {
    case Effect::Rainbow:    return 33;
    case Effect::Breathing:  return 33;
    case Effect::Pulse:      return 33;
    case Effect::Scanner:    return 60;
    case Effect::Wipe:       return 80;
    case Effect::Sparkle:    return 80;
    case Effect::Police:     return 80;
    case Effect::Fire:       return 60;
    case Effect::Chase:      return 60;
    case Effect::Theater:    return 100;
    case Effect::Listening:  return 33;
    case Effect::Thinking:   return 80;
    case Effect::Talking:    return 33;
    case Effect::Recording:  return 100;
    default:                 return 0;
    }
}

void tick_task(void *) {
    // Default tick period if no animated effect is current.
    EffectState local;
    while (true) {
        portENTER_CRITICAL(&g_eff_lock);
        local = g_eff;
        bool dirty = g_eff_dirty;
        g_eff_dirty = false;
        portEXIT_CRITICAL(&g_eff_lock);

        bool animate = effect_animates(local.effect);
        if (animate || dirty) {
            render_frame(local);
            if (animate) {
                portENTER_CRITICAL(&g_eff_lock);
                g_eff.frame = local.frame + 1;
                portEXIT_CRITICAL(&g_eff_lock);
            }
        }
        uint16_t sleep_ms = animate ? local.speed_ms : 50;   // 50 ms idle poll
        if (sleep_ms == 0) sleep_ms = 33;
        vTaskDelay(pdMS_TO_TICKS(sleep_ms));
    }
}

struct NamedEffect { const char *name; Effect eff; };
constexpr NamedEffect EFFECT_TABLE[] = {
    {"off",       Effect::Off},
    {"solid",     Effect::Solid},
    {"rainbow",   Effect::Rainbow},
    {"breathing", Effect::Breathing},
    {"pulse",     Effect::Pulse},
    {"scanner",   Effect::Scanner},
    {"wipe",      Effect::Wipe},
    {"sparkle",   Effect::Sparkle},
    {"police",    Effect::Police},
    {"fire",      Effect::Fire},
    {"chase",     Effect::Chase},
    {"theater",   Effect::Theater},
    {"listening", Effect::Listening},
    {"thinking",  Effect::Thinking},
    {"talking",   Effect::Talking},
    {"recording", Effect::Recording},
};

}  // namespace

esp_err_t led_init(i2c_master_bus_handle_t i2c_bus) {
    if (!i2c_bus) return ESP_ERR_INVALID_ARG;
    i2c_device_config_t cfg = {};
    cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    cfg.device_address  = PY32_ADDR;
    cfg.scl_speed_hz    = I2C_HZ;
    esp_err_t err = i2c_master_bus_add_device(i2c_bus, &cfg, &g_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "py32 bus_add_device: %s", esp_err_to_name(err));
        return err;
    }
    uint8_t version = 0;
    if (read_reg(REG_VERSION, &version) != ESP_OK || version == 0 || version == 0xFF) {
        ESP_LOGW(TAG, "PY32 not present (version=0x%02x) — LED disabled", version);
        i2c_master_bus_rm_device(g_dev);
        g_dev = nullptr;
        return ESP_ERR_NOT_FOUND;
    }
    err = write_reg(REG_LED_CFG, LED_COUNT & 0x3F);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set LED_CFG count: %s", esp_err_to_name(err));
        return err;
    }
    // Initial black state so the ring isn't whatever the chip booted with.
    led_set_all(0, 0, 0);
    // Spawn the animation tick task. 4 KB stack covers sinf/fmodf + I2C
    // path comfortably. Core 0 (away from speaker/decoder on core 1).
    BaseType_t ok = xTaskCreatePinnedToCore(tick_task, "led_fx", 4096, nullptr,
                                              tskIDLE_PRIORITY + 2, &g_tick_task, 0);
    if (ok != pdPASS) {
        ESP_LOGW(TAG, "led_fx task spawn failed — effects disabled");
    }
    ESP_LOGI(TAG, "led ring ready (PY32 0x%02x, version=0x%02x, %d leds)",
             PY32_ADDR, version, (int)LED_COUNT);
    return ESP_OK;
}

esp_err_t led_set_pixel(uint8_t index, uint8_t r, uint8_t g, uint8_t b) {
    if (!g_dev || index >= LED_COUNT) return ESP_ERR_INVALID_ARG;
    // Single-pixel writes pause any running effect — caller is asserting
    // a per-pixel state. led_set_effect can re-arm later.
    portENTER_CRITICAL(&g_eff_lock);
    g_eff.effect = Effect::Off;
    portEXIT_CRITICAL(&g_eff_lock);
    uint16_t c565 = rgb_to_565_le(r, g, b);
    uint8_t data[2] = { (uint8_t)(c565 & 0xFF), (uint8_t)((c565 >> 8) & 0xFF) };
    esp_err_t err = write_regs((uint8_t)(REG_LED_RAM_START + index * 2), data, 2);
    if (err != ESP_OK) return err;
    return refresh();
}

esp_err_t led_set_servo_power(bool enable) {
    if (!g_dev) return ESP_ERR_INVALID_STATE;
    // Pin 0: set as output AND drive the level. We RMW the low byte of
    // both the mode and output registers so we don't disturb the other
    // 7 pins (LED chip uses some of them as inputs).
    uint8_t mode_l = 0, out_l = 0;
    if (read_reg(REG_GPIO_M_L, &mode_l) != ESP_OK) return ESP_FAIL;
    if (read_reg(REG_GPIO_O_L, &out_l)  != ESP_OK) return ESP_FAIL;
    mode_l |= 0x01;                          // pin 0 → output
    if (enable) out_l |=  0x01;
    else        out_l &= ~0x01;
    esp_err_t err = write_reg(REG_GPIO_M_L, mode_l);
    if (err != ESP_OK) return err;
    err = write_reg(REG_GPIO_O_L, out_l);
    if (err != ESP_OK) return err;
    ESP_LOGI(TAG, "servo power %s (PY32 GPIO0 %s)",
             enable ? "on" : "off",
             enable ? "HIGH" : "LOW");
    return ESP_OK;
}

esp_err_t led_set_all(uint8_t r, uint8_t g, uint8_t b) {
    if (!g_dev) return ESP_ERR_INVALID_STATE;
    // Route through the effect engine as Solid so animation cancels cleanly.
    portENTER_CRITICAL(&g_eff_lock);
    g_eff.effect    = Effect::Solid;
    g_eff.r         = r;
    g_eff.g         = g;
    g_eff.b         = b;
    g_eff.speed_ms  = 0;
    g_eff.frame     = 0;
    g_eff_dirty     = true;
    portEXIT_CRITICAL(&g_eff_lock);
    uint16_t c565 = rgb_to_565_le(r, g, b);
    uint8_t blob[LED_COUNT * 2];
    for (int i = 0; i < LED_COUNT; ++i) {
        blob[i * 2 + 0] = (uint8_t)(c565 & 0xFF);
        blob[i * 2 + 1] = (uint8_t)((c565 >> 8) & 0xFF);
    }
    esp_err_t err = write_regs(REG_LED_RAM_START, blob, sizeof(blob));
    if (err != ESP_OK) return err;
    return refresh();
}

esp_err_t led_set_effect(const char *name, uint8_t r, uint8_t g, uint8_t b,
                         uint16_t speed_ms) {
    if (!g_dev || !name) return ESP_ERR_INVALID_ARG;
    Effect e = Effect::Solid;
    bool found = false;
    for (const auto &row : EFFECT_TABLE) {
        if (strcmp(row.name, name) == 0) { e = row.eff; found = true; break; }
    }
    if (!found) return ESP_ERR_NOT_FOUND;
    if (speed_ms == 0) speed_ms = default_speed_ms(e);
    portENTER_CRITICAL(&g_eff_lock);
    g_eff.effect    = e;
    g_eff.r         = r;
    g_eff.g         = g;
    g_eff.b         = b;
    g_eff.speed_ms  = speed_ms;
    g_eff.frame     = 0;
    g_eff_dirty     = true;
    portEXIT_CRITICAL(&g_eff_lock);
    ESP_LOGI(TAG, "effect=%s rgb=(%u,%u,%u) speed_ms=%u",
             name, r, g, b, (unsigned)speed_ms);
    return ESP_OK;
}

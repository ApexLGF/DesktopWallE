// Si12T touch — see touch.h. Ported from m5stack/StackChan's
// firmware/main/hal/drivers/Si12T/Si12T.cpp. We keep the same register
// init sequence (which is opaque but works — confirmed by M5Stack)
// and add edge-detection + cooldown on top.

#include "touch.h"

#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "touch";

namespace {

constexpr uint8_t  SI12T_ADDR        = 0x68;
constexpr uint32_t I2C_HZ            = 100000;
constexpr int      I2C_TIMEOUT_MS    = 50;

// Register map — names match the datasheet's "Si12 Touch" doc.
constexpr uint8_t  REG_SENS1         = 0x02;
constexpr uint8_t  REG_SENS2         = 0x03;
constexpr uint8_t  REG_SENS3         = 0x04;
constexpr uint8_t  REG_SENS4         = 0x05;
constexpr uint8_t  REG_SENS5         = 0x06;
constexpr uint8_t  REG_CTRL1         = 0x08;
constexpr uint8_t  REG_CTRL2         = 0x09;
constexpr uint8_t  REG_REF_RST1      = 0x0A;
constexpr uint8_t  REG_REF_RST2      = 0x0B;
constexpr uint8_t  REG_CH_HOLD1      = 0x0C;
constexpr uint8_t  REG_CH_HOLD2      = 0x0D;
constexpr uint8_t  REG_CAL_HOLD1     = 0x0E;
constexpr uint8_t  REG_CAL_HOLD2     = 0x0F;
constexpr uint8_t  REG_OUTPUT1       = 0x10;

// Sensitivity LOW-type, level 3 — same as M5Stack ref. Each nibble
// drives one pair of channels. 0x33 = "level 3" in both halves.
constexpr uint8_t  SENS_LOW_LV3      = 0x33;

constexpr int      POLL_MS           = 50;
constexpr int      COOLDOWN_MS       = 400;

i2c_master_dev_handle_t g_dev        = nullptr;
TaskHandle_t            g_task       = nullptr;
volatile touch_tap_cb_t g_cb         = nullptr;
volatile bool           g_present    = false;

esp_err_t write_reg(uint8_t reg, uint8_t value) {
    uint8_t buf[2] = { reg, value };
    return i2c_master_transmit(g_dev, buf, sizeof(buf), I2C_TIMEOUT_MS);
}

esp_err_t read_reg(uint8_t reg, uint8_t *out) {
    return i2c_master_transmit_receive(g_dev, &reg, 1, out, 1, I2C_TIMEOUT_MS);
}

esp_err_t setup_chip() {
    esp_err_t err = ESP_OK;
    // Enable all channels (CH 1-8 and CH 9). Writing 0 to *_HOLD enables.
    err |= write_reg(REG_REF_RST1,  0x00);
    err |= write_reg(REG_REF_RST2,  0x00);
    err |= write_reg(REG_CH_HOLD1,  0x00);
    err |= write_reg(REG_CH_HOLD2,  0x00);
    err |= write_reg(REG_CAL_HOLD1, 0x00);
    err |= write_reg(REG_CAL_HOLD2, 0x00);
    // CTRL2: S/W reset enable then sleep mode enable. The two writes
    // pulse a reset, per the M5Stack reference.
    err |= write_reg(REG_CTRL2,     0x0F);
    err |= write_reg(REG_CTRL2,     0x07);
    // CTRL1: auto-mode, FTC=01, interrupt on mid/high, response 4 (2+2).
    err |= write_reg(REG_CTRL1,     0x22);
    // Sensitivity (low-type, level 3) on the 5 sens registers that
    // cover our 3 channels plus headroom.
    err |= write_reg(REG_SENS1,     SENS_LOW_LV3);
    err |= write_reg(REG_SENS2,     SENS_LOW_LV3);
    err |= write_reg(REG_SENS3,     SENS_LOW_LV3);
    err |= write_reg(REG_SENS4,     SENS_LOW_LV3);
    err |= write_reg(REG_SENS5,     SENS_LOW_LV3);
    return err;
}

void parse_pads(uint8_t raw, uint8_t pads[3]) {
    // OUTPUT1 packs pads as 2-bit fields starting at bit 0:
    //   bits 1:0 = pad 0, bits 3:2 = pad 1, bits 5:4 = pad 2.
    // Values 0=NONE, 1=LOW, 2=MID, 3=HIGH.
    pads[0] = (raw >> 0) & 0x03;
    pads[1] = (raw >> 2) & 0x03;
    pads[2] = (raw >> 4) & 0x03;
}

void poll_task(void *arg) {
    (void)arg;
    uint8_t prev[3] = { 0, 0, 0 };
    TickType_t last_fire_tick = 0;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
        uint8_t raw = 0;
        if (read_reg(REG_OUTPUT1, &raw) != ESP_OK) {
            // I2C glitch — just retry next tick. Don't spam logs.
            continue;
        }
        uint8_t cur[3] = { 0, 0, 0 };
        parse_pads(raw, cur);

        bool in_cooldown =
            (xTaskGetTickCount() - last_fire_tick) < pdMS_TO_TICKS(COOLDOWN_MS);

        // Edge: any pad that just transitioned from NONE → ≥LOW counts
        // as a tap. We OR them so a hand covering all three pads only
        // fires once.
        int fired = -1;
        int fired_count = 0;
        for (int i = 0; i < 3; ++i) {
            if (prev[i] == 0 && cur[i] > 0) {
                if (fired < 0) fired = i;
                ++fired_count;
            }
        }
        memcpy(prev, cur, sizeof(prev));

        if (fired >= 0 && !in_cooldown) {
            last_fire_tick = xTaskGetTickCount();
            int idx = (fired_count > 1) ? -1 : fired;
            ESP_LOGI(TAG, "tap pad=%d (raw=0x%02x)", idx, raw);
            if (g_cb) g_cb(idx);
        }
    }
}

}  // namespace

void touch_register_tap_cb(touch_tap_cb_t cb) {
    g_cb = cb;
}

bool touch_is_present(void) { return g_present; }

esp_err_t touch_init(i2c_master_bus_handle_t i2c_bus) {
    if (!i2c_bus) return ESP_ERR_INVALID_ARG;
    if (g_task != nullptr) return ESP_ERR_INVALID_STATE;  // already initialised
    i2c_device_config_t cfg = {};
    cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    cfg.device_address  = SI12T_ADDR;
    cfg.scl_speed_hz    = I2C_HZ;
    esp_err_t err = i2c_master_bus_add_device(i2c_bus, &cfg, &g_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "bus_add_device: %s", esp_err_to_name(err));
        return err;
    }
    // Probe — a missing chip will NACK; bail without spawning a task.
    uint8_t probe = 0;
    if (read_reg(REG_OUTPUT1, &probe) != ESP_OK) {
        ESP_LOGW(TAG, "Si12T not present (0x%02x NACK) — touch disabled",
                 SI12T_ADDR);
        i2c_master_bus_rm_device(g_dev);
        g_dev = nullptr;
        return ESP_ERR_NOT_FOUND;
    }
    if (setup_chip() != ESP_OK) {
        ESP_LOGW(TAG, "Si12T setup failed — touch disabled");
        i2c_master_bus_rm_device(g_dev);
        g_dev = nullptr;
        return ESP_FAIL;
    }
    BaseType_t ok = xTaskCreatePinnedToCore(poll_task, "touch", 4096, nullptr,
                                              tskIDLE_PRIORITY + 2, &g_task, 0);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "task create failed");
        i2c_master_bus_rm_device(g_dev);
        g_dev = nullptr;
        return ESP_ERR_NO_MEM;
    }
    g_present = true;
    ESP_LOGI(TAG, "Si12T ready on I2C 0x%02x (3 pads, %d ms poll)",
             SI12T_ADDR, POLL_MS);
    return ESP_OK;
}

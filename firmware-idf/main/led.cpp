// LED ring — see led.h. Mirrors the protocol from
// Arduino-side `M5StackChan/src/drivers/PY32IOExpander/PY32IOExpander.cpp`
// trimmed to just the LED RAM region we need.

#include "led.h"

#include <string.h>

#include "esp_log.h"

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
    ESP_LOGI(TAG, "led ring ready (PY32 0x%02x, version=0x%02x, %d leds)",
             PY32_ADDR, version, (int)LED_COUNT);
    return ESP_OK;
}

esp_err_t led_set_pixel(uint8_t index, uint8_t r, uint8_t g, uint8_t b) {
    if (!g_dev || index >= LED_COUNT) return ESP_ERR_INVALID_ARG;
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

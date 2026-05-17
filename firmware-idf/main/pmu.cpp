// CoreS3 PMU bringup — see pmu.h.
//
// Register sequences lifted from M5Stack's official IDF firmware
// (firmware/main/hal/board/stackchan.cc, Pmic + Aw9523 ctors). Trimmed
// to just what's needed to power the audio analog chain; we keep all
// LDO rails enabled like stock so we don't accidentally break later
// peripherals when we add LCD / speaker / camera support.

#include "pmu.h"

#include "esp_log.h"

static const char *TAG = "pmu";

namespace {

constexpr uint8_t  AXP2101_ADDR = 0x34;
constexpr uint8_t  AW9523_ADDR  = 0x58;
constexpr uint32_t I2C_HZ       = 400000;
constexpr int      I2C_TIMEOUT_MS = 100;

i2c_master_bus_handle_t g_i2c_bus = nullptr;
int                     g_backlight_pct = 80;

esp_err_t reg_write(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t value) {
    uint8_t buf[2] = {reg, value};
    return i2c_master_transmit(dev, buf, sizeof(buf), I2C_TIMEOUT_MS);
}

esp_err_t reg_read(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t *value) {
    return i2c_master_transmit_receive(dev, &reg, 1, value, 1, I2C_TIMEOUT_MS);
}

esp_err_t init_axp2101(i2c_master_bus_handle_t bus) {
    i2c_master_dev_handle_t dev = nullptr;
    i2c_device_config_t cfg = {};
    cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    cfg.device_address  = AXP2101_ADDR;
    cfg.scl_speed_hz    = I2C_HZ;
    esp_err_t err = i2c_master_bus_add_device(bus, &cfg, &dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "AXP2101 bus_add_device: %s", esp_err_to_name(err));
        return err;
    }

    // The full M5Stack sequence in order (with comments based on the
    // AXP2101 datasheet + XPowersLib register map):
    //
    //   0x90: LDO enable mask
    //         bit 7 DLDO1 / bit 6 BLDO2 / bit 5 BLDO1 / bit 4 ALDO4
    //         bit 3 ALDO3 / bit 2 ALDO2 / bit 1 ALDO1 / bit 0 DLDO2
    //   0x94: ALDO3 setpoint (V = 0.5 + n*0.1)  → 28 = 3.3 V (mic analog)
    //   0x95: ALDO4 setpoint                    → 28 = 3.3 V (LCD)
    //   0x97: BLDO1 setpoint                    → 0b11110-2 = 0x1C
    //   0x27: shut-down threshold
    //   0x30: button / IRQ wake mask
    //   0x69: charging-LED mode (constant on while charging)
    uint8_t cur90 = 0;
    if (reg_read(dev, 0x90, &cur90) == ESP_OK) {
        cur90 |= 0b10110100;  // pre-set ALDO4, ALDO3, BLDO1, DLDO1 before final write
        reg_write(dev, 0x90, cur90);
    }
    reg_write(dev, 0x97, 0b11110 - 2);   // BLDO1 voltage
    reg_write(dev, 0x69, 0b00110101);    // charge LED
    reg_write(dev, 0x30, 0b00111111);    // adc / button wake
    reg_write(dev, 0x90, 0xBF);          // final LDO mask: everything ON except BLDO2
    reg_write(dev, 0x94, 33 - 5);        // ALDO3 = 3.3 V  ← mic analog supply
    reg_write(dev, 0x95, 33 - 5);        // ALDO4 = 3.3 V  ← LCD
    reg_write(dev, 0x27, 0x00);

    // Sanity-readback so we know I²C worked end-to-end.
    uint8_t v90 = 0, v94 = 0, v95 = 0;
    reg_read(dev, 0x90, &v90);
    reg_read(dev, 0x94, &v94);
    reg_read(dev, 0x95, &v95);
    ESP_LOGI(TAG, "AXP2101: reg90=0x%02x reg94=0x%02x reg95=0x%02x", v90, v94, v95);

    i2c_master_bus_rm_device(dev);
    return ESP_OK;
}

esp_err_t init_aw9523(i2c_master_bus_handle_t bus) {
    i2c_master_dev_handle_t dev = nullptr;
    i2c_device_config_t cfg = {};
    cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    cfg.device_address  = AW9523_ADDR;
    cfg.scl_speed_hz    = I2C_HZ;
    esp_err_t err = i2c_master_bus_add_device(bus, &cfg, &dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "AW9523 bus_add_device: %s", esp_err_to_name(err));
        return err;
    }

    // Verbatim from M5Stack's Aw9523 ctor:
    //   0x02  P0 output latch
    //   0x03  P1 output latch
    //   0x04  CONFIG_P0  (1=input, 0=output)
    //   0x05  CONFIG_P1
    //   0x11  Global Control Register (P0 push-pull instead of open-drain)
    //   0x12  LEDMODE_P0  (all bits 1 = pure GPIO, not constant-current)
    //   0x13  LEDMODE_P1
    reg_write(dev, 0x02, 0b00000111);
    reg_write(dev, 0x03, 0b10001111);
    reg_write(dev, 0x04, 0b00011000);
    reg_write(dev, 0x05, 0b00001100);
    reg_write(dev, 0x11, 0b00010000);
    reg_write(dev, 0x12, 0b11111111);
    reg_write(dev, 0x13, 0b11111111);

    uint8_t id = 0;
    reg_read(dev, 0x10, &id);  // 0x10 = ID register, should read 0x23
    ESP_LOGI(TAG, "AW9523: chip_id=0x%02x (expect 0x23)", id);

    i2c_master_bus_rm_device(dev);
    return ESP_OK;
}

}  // namespace

esp_err_t pmu_init(i2c_master_bus_handle_t i2c_bus) {
    esp_err_t err;
    g_i2c_bus = i2c_bus;
    err = init_axp2101(i2c_bus);
    if (err != ESP_OK) return err;
    err = init_aw9523(i2c_bus);
    if (err != ESP_OK) return err;
    ESP_LOGI(TAG, "PMU bringup done (ALDO3=3.3V, AW9523 outputs configured)");
    return ESP_OK;
}

static esp_err_t axp_open(i2c_master_dev_handle_t *out) {
    if (!g_i2c_bus) return ESP_FAIL;
    i2c_device_config_t cfg = {};
    cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    cfg.device_address  = AXP2101_ADDR;
    cfg.scl_speed_hz    = I2C_HZ;
    return i2c_master_bus_add_device(g_i2c_bus, &cfg, out);
}

static esp_err_t axp_read(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t *value) {
    return i2c_master_transmit_receive(dev, &reg, 1, value, 1, I2C_TIMEOUT_MS);
}

static esp_err_t axp_write(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t value) {
    uint8_t buf[2] = {reg, value};
    return i2c_master_transmit(dev, buf, sizeof(buf), I2C_TIMEOUT_MS);
}

esp_err_t pmu_battery_read(int *voltage_mv, int *percent, bool *charging,
                           int *vbus_mv) {
    i2c_master_dev_handle_t dev = nullptr;
    if (axp_open(&dev) != ESP_OK || !dev) return ESP_FAIL;

    uint8_t hi = 0, lo = 0;
    esp_err_t r;
    int vbat = 0, vbus = 0, pct = -1;
    bool chg = false;

    r = axp_read(dev, 0x34, &hi);
    if (r != ESP_OK) {
        i2c_master_bus_rm_device(dev);
        return r;
    }
    axp_read(dev, 0x35, &lo);
    vbat = ((int)(hi & 0x3F) << 8) | lo;

    axp_read(dev, 0x56, &hi);
    axp_read(dev, 0x57, &lo);
    vbus = ((int)(hi & 0x3F) << 8) | lo;

    uint8_t soc = 0;
    if (axp_read(dev, 0xA4, &soc) == ESP_OK) pct = soc;

    uint8_t st0 = 0, st1 = 0;
    axp_read(dev, 0x00, &st0);
    axp_read(dev, 0x01, &st1);
    chg = (st0 & 0x04) && (((st1 >> 5) & 0x03) != 0);

    if (voltage_mv) *voltage_mv = vbat;
    if (vbus_mv)    *vbus_mv    = vbus;
    if (percent)    *percent    = pct;
    if (charging)   *charging   = chg;

    i2c_master_bus_rm_device(dev);
    return ESP_OK;
}

esp_err_t pmu_set_backlight(int pct) {
    if (pct < 0 || pct > 100) return ESP_ERR_INVALID_ARG;
    if (!g_i2c_bus) return ESP_FAIL;
    i2c_master_dev_handle_t dev = nullptr;
    if (axp_open(&dev) != ESP_OK || !dev) return ESP_FAIL;

    uint8_t code = 0;
    if (pct > 0) {
        code = (uint8_t)(4 + (pct * 24 + 50) / 100);
        if (code > 28) code = 28;
    }
    axp_write(dev, 0x99, code);

    uint8_t cur = 0;
    axp_read(dev, 0x90, &cur);
    if (pct > 0) cur |= 0x80; else cur &= ~0x80;
    axp_write(dev, 0x90, cur);

    g_backlight_pct = pct;
    i2c_master_bus_rm_device(dev);
    ESP_LOGI(TAG, "backlight = %d%% (DLDO1 code=%d)", pct, code);
    return ESP_OK;
}

int pmu_get_backlight(void) { return g_backlight_pct; }

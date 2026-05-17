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
    err = init_axp2101(i2c_bus);
    if (err != ESP_OK) return err;
    err = init_aw9523(i2c_bus);
    if (err != ESP_OK) return err;
    ESP_LOGI(TAG, "PMU bringup done (ALDO3=3.3V, AW9523 outputs configured)");
    return ESP_OK;
}

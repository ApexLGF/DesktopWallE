// StackChan 12-LED ring controlled by the PY32 IO expander on the base.
//
// The PY32 sits on the same I2C bus as the AXP2101/AW9523/ES7210 cluster
// (I2C_NUM_1, SDA=GPIO12, SCL=GPIO11) at 7-bit address 0x6F. It exposes
// LED color RAM at register 0x30+ (16-bit RGB565 LE per LED) and latches
// the change via bit 6 of REG_LED_CFG (0x24).

#pragma once

#include <stdint.h>

#include "esp_err.h"
#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t led_init(i2c_master_bus_handle_t i2c_bus);

// Set one LED's color (index 0..11). Sends to the chip immediately and
// triggers the refresh latch.
esp_err_t led_set_pixel(uint8_t index, uint8_t r, uint8_t g, uint8_t b);

// Set all 12 LEDs to the same color in one transaction.
esp_err_t led_set_all(uint8_t r, uint8_t g, uint8_t b);

// Enable / disable the servo power rail. The PY32 also gates the head
// servo bus via its GPIO0; without this set HIGH the SCS servos are
// dark on the wire and uart writes have no effect.
esp_err_t led_set_servo_power(bool enable);

#ifdef __cplusplus
}
#endif

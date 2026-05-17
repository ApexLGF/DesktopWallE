// Minimal CoreS3 PMU bringup.
//
// The CoreS3 audio chain needs two chips powered/configured before the
// ES7210 mic ADC's analog preamp gets a usable supply:
//
//   AXP2101 @ I2C 0x34  (PMU)
//     ALDO3 @ 3.3 V → ES7210 analog VDD       ← critical for mic
//     ALDO4 @ 3.3 V → ILI9342C LCD            (we don't need yet)
//     DLDO1         → LCD backlight           (we don't need yet)
//
//   AW9523 @ I2C 0x58  (GPIO expander)
//     P0/P1 set as outputs with the same defaults the M5Stack stock
//     firmware uses — releases AW88298 (speaker amp) reset etc.
//
// This file does *only* the minimum needed for mic input. If the
// firmware later wants the LCD / speaker / camera it'll need more
// rails enabled and AW9523 lines toggled.

#pragma once

#include "esp_err.h"
#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

// Initialize AXP2101 + AW9523 on the supplied i2c bus.
// Call BEFORE mic_init() — the ES7210 will respond on I²C even without
// these rails on (digital side is fed from VBUS pass-through) but its
// mic PGA will be starved and the output will sit at the noise floor.
esp_err_t pmu_init(i2c_master_bus_handle_t i2c_bus);

#ifdef __cplusplus
}
#endif

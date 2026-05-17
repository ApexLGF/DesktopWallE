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

// Read battery state from the AXP2101 ADC. All output parameters may be
// NULL if a field isn't wanted. Returns ESP_OK on a clean register read,
// ESP_FAIL otherwise (e.g. pmu_init wasn't called or the I²C transaction
// errored). The AXP2101's battery SoC counter only updates when charge
// curves complete, so percent can lag voltage by a charge cycle.
//
//   voltage_mv  battery terminal voltage in millivolts (3300..4200 typical)
//   percent     fuel-gauge state-of-charge 0..100
//   charging    true while VBUS present and charge LED active
//   vbus_mv     USB / VBUS input voltage (informational)
esp_err_t pmu_battery_read(int *voltage_mv, int *percent, bool *charging,
                           int *vbus_mv);

// Set the LCD backlight brightness as integer percent [0, 100]. Drives
// DLDO1 setpoint on the AXP2101 (the backlight rail used by CoreS3).
// 0 turns the backlight off; 100 ≈ 3.3 V. Returns ESP_ERR_INVALID_ARG
// for out-of-range values.
esp_err_t pmu_set_backlight(int pct);
int       pmu_get_backlight(void);

#ifdef __cplusplus
}
#endif

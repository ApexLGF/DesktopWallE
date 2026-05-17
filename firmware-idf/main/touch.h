// Si12T capacitive touch — 3 pads on top of the StackChan head.
//
// Hardware: Silicon Labs Si12T on I2C 0x68, sharing the main I2C bus
// with PMU / mic / speaker / LED. Datasheet exposes 9 channels but
// M5Stack wires only 3 (forehead-left / forehead-mid / forehead-right).
//
// Protocol mirrors m5stack/StackChan firmware:
//   firmware/main/hal/drivers/Si12T/Si12T.{h,cpp}.
// Register OUTPUT1 (0x10) packs the 3 pad levels in 2 bits each:
//   NONE=0, LOW=1, MID=2, HIGH=3.
//
// What this module does:
//   - polls OUTPUT1 every 50 ms in a dedicated task
//   - debounces per-pad and fires `tap_cb` on the NONE→touched edge,
//     with a 400 ms global cooldown to avoid double-fires
// What it does NOT do (kept simple on purpose):
//   - swipe gesture recognition (M5Stack's IDLE→TOUCHED→SWIPING state
//     machine). We only need taps for "interrupt the robot."

#pragma once

#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// pad_index is 0..2 (0=left, 1=mid, 2=right) or -1 when multiple
// fired in the same poll (caller can usually treat that as "any").
typedef void (*touch_tap_cb_t)(int pad_index);

esp_err_t touch_init(i2c_master_bus_handle_t i2c_bus);

// Replace the tap callback. NULL disables tap notifications without
// shutting down the polling task. Must be called BEFORE touch_init()
// (the polling task reads it without a barrier); hot-swap after init
// is not supported.
void      touch_register_tap_cb(touch_tap_cb_t cb);

// True if Si12T responded to the init probe. Used by main.cpp to
// decide whether to spam the boot log about a missing chip.
bool      touch_is_present(void);

#ifdef __cplusplus
}
#endif

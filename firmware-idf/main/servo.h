// Feetech SCS-bus servo driver — UART1 @ 1 Mbps, TX=GPIO6, RX=GPIO7.
//
// StackChan has two head servos on the SCS bus:
//   ID=1  yaw   (left/right)
//   ID=2  pitch (up/down)
// Both use the standard SCS memory map (positions in raw 0..1023, where
// ~512 is mechanical zero). The "stackchan unit" convention used by
// the old Arduino firmware is 10 = 1 degree; we accept those and
// translate to raw SCS units internally.
//
// Protocol overview (single write packet, no ack-read for simplicity):
//   0xFF 0xFF  ID  LEN  INST(0x03)  ADDR  DATA[N]  CHECKSUM
//   LEN      = N + 3
//   CHECKSUM = ~(ID + LEN + INST + ADDR + sum(DATA))  & 0xFF
//
// We never read back from the bus, so a missing or stuck servo is a
// silent failure — but for the action-emit use case that's fine.

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// One-shot UART bring-up. Idempotent — safe to call twice.
esp_err_t servo_init(void);

// Send a position command. `pos` is the raw SCS register value (0..1023).
// `time_ms` is movement time hint to the servo (0 = unconstrained).
// `speed` is target velocity 0..1023 (0 = "as fast as possible").
esp_err_t servo_write_pos(uint8_t id, uint16_t pos, uint16_t time_ms, uint16_t speed);

// Enable / disable torque. With torque off the servo goes limp; useful
// for "sleep" pose without holding current.
esp_err_t servo_torque(uint8_t id, bool enabled);

// High-level pose move in StackChan units.
//   x: yaw,   signed [-1280, 1280] (0 = forward).
//   y: pitch,        [   0,  850] (~425 = horizontal).
//   speed:           [   0, 1000] (clamped). 200 ≈ natural turn.
// Sends one packet to each servo back-to-back.
void servo_move(int x, int y, int speed);

// Neutral "look forward" pose. Equivalent to Motion.goHome() in the
// old firmware. Convenient for boot, dance reset, error-recovery.
void servo_home(void);

// Whether servo_init() has completed successfully. Used by handlers
// to short-circuit when no servos are connected.
bool servo_is_ready(void);

// Run a named gesture inline. Returns ESP_ERR_NOT_FOUND for unknown
// names. Names match the old Arduino firmware:
//   home, nod, shake, yes, no,
//   look_up, look_down, look_left, look_right, look_around,
//   dance, surprised, sleep, wake, panic, peek, bow,
//   tilt_left, tilt_right
// Each gesture is a short blocking sequence of servo_write_pos +
// vTaskDelay (typical 0.5–3 s total).
esp_err_t servo_action(const char *name);

// Read the most recently commanded yaw / pitch (in StackChan units).
// Both pointers may be NULL. Values reflect the last servo_move()
// argument — these are setpoints, not measured positions (we never
// read back from the bus).
void servo_get_pos(int *x, int *y);

// Enable / disable the periodic "look around" idle motion. When on,
// an internal task wakes every 4–9 s and issues a small random head
// move. Yaw range ±450 sc-units, pitch 200..600. Disabled by default
// so it never fights live agent commands.
void servo_set_idle(bool enabled);
bool servo_get_idle(void);

// Enable / disable the subtle "breathing" pitch oscillation. The
// internal task modulates pitch by ±20 sc-units around the most
// recent commanded Y at ~60 ms cadence. Cheap and convincing —
// makes the robot look alive between explicit commands.
void servo_set_breathing(bool enabled);
bool servo_get_breathing(void);

// Halt motion. Disables idle + breathing and re-issues the current
// goal position with default speed, which on Feetech SCS servos
// terminates any in-flight tween. Does NOT cancel a blocking gesture
// running on the caller's task (servo_action is synchronous).
void servo_stop_motion(void);

#ifdef __cplusplus
}
#endif

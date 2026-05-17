// Minimal CoreS3 LCD output (ILI9342C, 320×240, SPI3).
//
// Just enough to give the user a visible cue when the bridge has
// opened the mic and a coarse indication of agent state. No LVGL,
// no Chinese fonts — full-screen color background + a big ASCII
// status word. Easy to grow into a fuller display layer later.

#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LCD_STATE_BOOT      = 0,  // black + "BOOT" while we come up
    LCD_STATE_WIFI      = 1,  // dark blue + "WIFI"
    LCD_STATE_BRIDGE    = 2,  // teal + "BRIDGE"
    LCD_STATE_IDLE      = 3,  // black + "IDLE"
    LCD_STATE_LISTENING = 4,  // bright blue + "LISTEN"
    LCD_STATE_HEARD     = 5,  // green + "HEARD"
    LCD_STATE_THINKING  = 6,  // purple + "THINK"
    LCD_STATE_SPEAKING  = 7,  // orange + "TALK"
    LCD_STATE_ERROR     = 8,  // red + "ERR"
} lcd_state_t;

esp_err_t lcd_init(void);
void      lcd_set_state(lcd_state_t s);

// While in THINKING state, render the elapsed seconds counter (the bridge
// pushes `show_text {title:"思考中… Ns秒"}` every 2 s while Hermes runs).
// `seconds` < 0 clears the counter. Calling outside THINKING state is a
// no-op so we don't accidentally repaint over a fresher transition.
void      lcd_set_think_elapsed(int seconds);

#ifdef __cplusplus
}
#endif

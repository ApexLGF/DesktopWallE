// CoreS3 LCD — ILI9342C via SPI3, raw esp_lcd panel.
//
// Big colored background + scaled 5×7 ASCII bitmap font for the status
// word. PSRAM-backed framebuffer (320×240×2 = 153.6 KB) so we can paint
// the whole screen in one bus transaction and avoid tearing.

#include "lcd.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_ili9341.h"

static const char *TAG = "lcd";

namespace {

constexpr spi_host_device_t SPI_HOST_LCD = SPI3_HOST;
constexpr gpio_num_t  PIN_MOSI = GPIO_NUM_37;
constexpr gpio_num_t  PIN_SCLK = GPIO_NUM_36;
constexpr gpio_num_t  PIN_CS   = GPIO_NUM_3;
constexpr gpio_num_t  PIN_DC   = GPIO_NUM_35;

constexpr int W = 320;
constexpr int H = 240;
constexpr int BAND_H = 30;     // 320*30*2 = 19.2 KB per SPI shadow alloc

esp_lcd_panel_handle_t    g_panel     = nullptr;
esp_lcd_panel_io_handle_t g_io        = nullptr;
SemaphoreHandle_t         g_trans_sem = nullptr;  // released by on_color_trans_done
uint16_t                 *g_fb        = nullptr;     // PSRAM framebuffer, RGB565 BE
lcd_state_t               g_state     = LCD_STATE_BOOT;
esp_timer_handle_t        g_idle_timer = nullptr;   // optional deferred IDLE flip

// Called from ISR when esp_lcd has finished pushing the last byte of a
// queued draw_bitmap. Lets us block the next paint until the bus is idle
// instead of guessing with a fixed sleep — which was racing the SPI queue
// and silently dropping subsequent draws.
bool IRAM_ATTR on_color_trans_done(esp_lcd_panel_io_handle_t,
                                    esp_lcd_panel_io_event_data_t *,
                                    void *) {
    BaseType_t hpw = pdFALSE;
    xSemaphoreGiveFromISR(g_trans_sem, &hpw);
    return hpw == pdTRUE;
}

// Push the framebuffer to the panel in horizontal bands. A single
// 320×240×2 = 153.6 KB transaction forces spi_master to allocate an
// equally-large shadow buffer in DMA-capable internal RAM (the
// framebuffer is in PSRAM, which the bus can't DMA from in this
// config). Under steady load that alloc fails — setup_dma_priv_buffer
// — and the paint is dropped. 30 rows = 19.2 KB per chunk fits even
// under heavy heap fragmentation. Bands are sent serially, gated on
// `g_trans_sem`, leaving the sem in "available" state at return so the
// next lcd_set_state can immediately take it.
esp_err_t draw_fb_banded() {
    for (int y = 0; y < H; y += BAND_H) {
        int h = (y + BAND_H > H) ? (H - y) : BAND_H;
        if (xSemaphoreTake(g_trans_sem, pdMS_TO_TICKS(500)) != pdTRUE) {
            ESP_LOGW(TAG, "band y=%d: sem timeout", y);
            xSemaphoreGive(g_trans_sem);
            return ESP_ERR_TIMEOUT;
        }
        esp_err_t err = esp_lcd_panel_draw_bitmap(g_panel, 0, y, W, y + h,
                                                    g_fb + y * W);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "band y=%d draw_bitmap: %s", y, esp_err_to_name(err));
            xSemaphoreGive(g_trans_sem);
            return err;
        }
    }
    // Wait for the last band's callback so subsequent set_state finds
    // the sem available without an extra wait.
    if (xSemaphoreTake(g_trans_sem, pdMS_TO_TICKS(500)) != pdTRUE) {
        ESP_LOGW(TAG, "final band wait: sem timeout");
    }
    xSemaphoreGive(g_trans_sem);
    return ESP_OK;
}

// Mini 5×7 ASCII font, only the characters we actually use.
// LSB-first bytes; 5 bits wide. Stored as 7 bytes per glyph.
struct Glyph { char c; uint8_t rows[7]; };

constexpr Glyph FONT[] = {
    {' ', {0,0,0,0,0,0,0}},
    {'A', {0x0E,0x11,0x11,0x1F,0x11,0x11,0x11}},
    {'B', {0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E}},
    {'C', {0x0E,0x11,0x10,0x10,0x10,0x11,0x0E}},
    {'D', {0x1E,0x11,0x11,0x11,0x11,0x11,0x1E}},
    {'E', {0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F}},
    {'F', {0x1F,0x10,0x10,0x1E,0x10,0x10,0x10}},
    {'G', {0x0E,0x11,0x10,0x17,0x11,0x11,0x0E}},
    {'H', {0x11,0x11,0x11,0x1F,0x11,0x11,0x11}},
    {'I', {0x0E,0x04,0x04,0x04,0x04,0x04,0x0E}},
    {'J', {0x07,0x02,0x02,0x02,0x02,0x12,0x0C}},
    {'K', {0x11,0x12,0x14,0x18,0x14,0x12,0x11}},
    {'L', {0x10,0x10,0x10,0x10,0x10,0x10,0x1F}},
    {'M', {0x11,0x1B,0x15,0x15,0x11,0x11,0x11}},
    {'N', {0x11,0x11,0x19,0x15,0x13,0x11,0x11}},
    {'O', {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E}},
    {'P', {0x1E,0x11,0x11,0x1E,0x10,0x10,0x10}},
    {'R', {0x1E,0x11,0x11,0x1E,0x14,0x12,0x11}},
    {'S', {0x0E,0x11,0x10,0x0E,0x01,0x11,0x0E}},
    {'T', {0x1F,0x04,0x04,0x04,0x04,0x04,0x04}},
    {'U', {0x11,0x11,0x11,0x11,0x11,0x11,0x0E}},
    {'W', {0x11,0x11,0x11,0x15,0x15,0x1B,0x11}},
    {'!', {0x04,0x04,0x04,0x04,0x04,0x00,0x04}},
    {'.', {0x00,0x00,0x00,0x00,0x00,0x00,0x04}},
    {'0', {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E}},
    {'1', {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E}},
    {'2', {0x0E,0x11,0x01,0x06,0x08,0x10,0x1F}},
    {'3', {0x1F,0x02,0x04,0x02,0x01,0x11,0x0E}},
    {'4', {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02}},
    {'5', {0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E}},
    {'6', {0x06,0x08,0x10,0x1E,0x11,0x11,0x0E}},
    {'7', {0x1F,0x01,0x02,0x04,0x08,0x08,0x08}},
    {'8', {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E}},
    {'9', {0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C}},
};

const uint8_t *glyph_rows(char c) {
    for (const auto &g : FONT) if (g.c == c) return g.rows;
    return FONT[0].rows;  // space if not found
}

uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
    uint16_t v = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
    // ILI9342C wants big-endian when bits_per_pixel=16 SPI; swap.
    return (v >> 8) | (v << 8);
}

void fb_fill(uint16_t color) {
    for (int i = 0; i < W * H; ++i) g_fb[i] = color;
}

// Draw glyph at top-left (x,y), scaled `s`×; foreground `fg`, bg untouched.
void fb_glyph(int x, int y, char c, int s, uint16_t fg) {
    const uint8_t *rows = glyph_rows(c);
    for (int row = 0; row < 7; ++row) {
        uint8_t bits = rows[row];
        for (int col = 0; col < 5; ++col) {
            if (bits & (1 << (4 - col))) {
                for (int dy = 0; dy < s; ++dy) {
                    int py = y + row * s + dy;
                    if (py < 0 || py >= H) continue;
                    for (int dx = 0; dx < s; ++dx) {
                        int px = x + col * s + dx;
                        if (px < 0 || px >= W) continue;
                        g_fb[py * W + px] = fg;
                    }
                }
            }
        }
    }
}

void fb_text_centered(const char *s, int y, int scale, uint16_t fg) {
    int len = (int)strlen(s);
    int glyph_w = 5 * scale + scale;     // 1-px gap
    int total = len * glyph_w - scale;
    int x = (W - total) / 2;
    for (int i = 0; i < len; ++i) {
        fb_glyph(x + i * glyph_w, y, s[i], scale, fg);
    }
}

struct Style { uint8_t r, g, b; const char *label; };

Style style_for(lcd_state_t s) {
    switch (s) {
    case LCD_STATE_BOOT:      return { 20,  20,  20, "BOOT" };
    case LCD_STATE_WIFI:      return { 10,  30, 100, "WIFI" };
    case LCD_STATE_BRIDGE:    return { 10,  90, 120, "BRIDGE" };
    case LCD_STATE_IDLE:      return {  5,   5,  20, "IDLE"   };
    case LCD_STATE_LISTENING: return { 30, 100, 220, "LISTEN" };
    case LCD_STATE_HEARD:     return { 30, 180,  60, "HEARD"  };
    case LCD_STATE_THINKING:  return {120,  70, 180, "THINK"  };
    case LCD_STATE_SPEAKING:  return {220, 140,  20, "TALK"   };
    case LCD_STATE_ERROR:     return {200,  50,  20, "ERR"    };
    case LCD_STATE_ASR:       return { 20, 160, 180, "ASR"    };
    case LCD_STATE_ASR_ERR:   return {180,  60,  40, "ASR ERR"};
    case LCD_STATE_FACE:      return {  0,   0,   0, ""       };
    }
    return { 0, 0, 0, "" };
}

int g_think_elapsed = -1;   // seconds; <0 = hide

// ── Drawing primitives over the PSRAM framebuffer ───────────────────────

inline void fb_pixel(int x, int y, uint16_t c) {
    if ((unsigned)x < (unsigned)W && (unsigned)y < (unsigned)H) g_fb[y * W + x] = c;
}

void fb_hline(int x, int y, int len, uint16_t c) {
    if ((unsigned)y >= (unsigned)H) return;
    if (x < 0) { len += x; x = 0; }
    if (x + len > W) len = W - x;
    if (len <= 0) return;
    uint16_t *row = g_fb + y * W + x;
    for (int i = 0; i < len; ++i) row[i] = c;
}

void fb_fill_rect(int x, int y, int w, int h, uint16_t c) {
    for (int j = 0; j < h; ++j) fb_hline(x, y + j, w, c);
}

void fb_fill_circle(int cx, int cy, int r, uint16_t c) {
    if (r <= 0) return;
    int r2 = r * r;
    for (int dy = -r; dy <= r; ++dy) {
        int yy = cy + dy;
        if ((unsigned)yy >= (unsigned)H) continue;
        int dx_max = (int)sqrtf((float)(r2 - dy * dy));
        fb_hline(cx - dx_max, yy, 2 * dx_max + 1, c);
    }
}

void fb_fill_round_rect(int x, int y, int w, int h, int r, uint16_t c) {
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;
    fb_fill_rect(x + r, y,     w - 2 * r, h,     c);
    fb_fill_rect(x,     y + r, r,         h - 2 * r, c);
    fb_fill_rect(x + w - r, y + r, r,     h - 2 * r, c);
    int r2 = r * r;
    for (int dy = -r; dy <= 0; ++dy) {
        int dx_max = (int)sqrtf((float)(r2 - dy * dy));
        fb_hline(x + r - dx_max,     y + r + dy,         dx_max + 1, c);   // top-left arc
        fb_hline(x + w - r,          y + r + dy,         dx_max + 1, c);   // top-right arc
        fb_hline(x + r - dx_max,     y + h - r - 1 - dy, dx_max + 1, c);   // bottom-left
        fb_hline(x + w - r,          y + h - r - 1 - dy, dx_max + 1, c);   // bottom-right
    }
}

void fb_line(int x0, int y0, int x1, int y1, uint16_t c) {
    int dx = abs(x1 - x0), dy = -abs(y1 - y0);
    int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    while (true) {
        fb_pixel(x0, y0, c);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

void fb_fill_triangle(int x0, int y0, int x1, int y1, int x2, int y2, uint16_t c) {
    // Sort vertices by y ascending: y0 ≤ y1 ≤ y2.
    if (y0 > y1) { int t = y0; y0 = y1; y1 = t; t = x0; x0 = x1; x1 = t; }
    if (y1 > y2) { int t = y1; y1 = y2; y2 = t; t = x1; x1 = x2; x2 = t; }
    if (y0 > y1) { int t = y0; y0 = y1; y1 = t; t = x0; x0 = x1; x1 = t; }
    auto draw_span = [&](int y, int a, int b) {
        if (a > b) { int t = a; a = b; b = t; }
        fb_hline(a, y, b - a + 1, c);
    };
    // Upper triangle (y0..y1) and lower triangle (y1..y2).
    int total_h = y2 - y0;
    if (total_h == 0) total_h = 1;
    for (int y = y0; y <= y2; ++y) {
        bool upper = y < y1 || y1 == y0;
        int a, b;
        // Long edge: y0→y2.
        a = x0 + (x2 - x0) * (y - y0) / total_h;
        if (upper) {
            int h = (y1 - y0); if (h == 0) h = 1;
            b = x0 + (x1 - x0) * (y - y0) / h;
        } else {
            int h = (y2 - y1); if (h == 0) h = 1;
            b = x1 + (x2 - x1) * (y - y1) / h;
        }
        draw_span(y, a, b);
    }
}

inline uint16_t rgb24_to_565(uint32_t rgb) {
    return rgb565((uint8_t)((rgb >> 16) & 0xFF),
                  (uint8_t)((rgb >>  8) & 0xFF),
                  (uint8_t)( rgb        & 0xFF));
}

// ── Face renderer (port of src/main.cpp's drawFace + presets) ───────────

struct FaceParams {
    int  eye_y       = 110;
    int  eye_r       = 28;
    int  eye_dx      = 60;
    int  mouth_y     = 185;
    int  mouth_w     = 70;
    int  mouth_h     = 18;
    bool left_closed  = false;
    bool right_closed = false;
    int  pupil_dx    = 0;
    int  pupil_dy    = 0;
    int  brow_angle  = 0;     // -1 angry, 0 neutral, +1 sad
    int  mouth_shape = 0;     // 0 line, 1 smile, 2 frown, 3 open, 4 dot, 5 cat
    bool show_tongue = false;
    bool show_z      = false;
    bool show_heart  = false;
    bool show_sweat  = false;
};

void face_preset(const char *name, FaceParams &p) {
    p = FaceParams();
    if (!name || strcmp(name, "neutral") == 0)        p.mouth_shape = 0;
    else if (strcmp(name, "happy")       == 0)        p.mouth_shape = 1;
    else if (strcmp(name, "smile")       == 0)      { p.mouth_shape = 1; p.eye_r = 24; }
    else if (strcmp(name, "love")        == 0)      { p.mouth_shape = 1; p.show_heart = true; p.eye_r = 22; }
    else if (strcmp(name, "sad")         == 0)      { p.mouth_shape = 2; p.brow_angle = 1; }
    else if (strcmp(name, "angry")       == 0)      { p.mouth_shape = 2; p.brow_angle = -1; p.eye_r = 24; }
    else if (strcmp(name, "surprised")   == 0)      { p.mouth_shape = 3; p.eye_r = 32; }
    else if (strcmp(name, "thinking")    == 0)      { p.mouth_shape = 4; p.pupil_dx = 12; p.pupil_dy = -8; p.brow_angle = -1; }
    else if (strcmp(name, "sleep")       == 0)      { p.left_closed = true; p.right_closed = true; p.mouth_shape = 4; p.show_z = true; }
    else if (strcmp(name, "wink_l")      == 0)      { p.left_closed = true; p.mouth_shape = 1; }
    else if (strcmp(name, "wink_r")      == 0)      { p.right_closed = true; p.mouth_shape = 1; }
    else if (strcmp(name, "stare")       == 0)      { p.mouth_shape = 0; p.eye_r = 30; }
    else if (strcmp(name, "dead")        == 0)      { p.mouth_shape = 0; p.pupil_dx = -8; }
    else if (strcmp(name, "embarrassed") == 0)      { p.mouth_shape = 1; p.show_sweat = true; p.brow_angle = 1; }
    else if (strcmp(name, "cat")         == 0)      { p.mouth_shape = 5; p.eye_r = 22; }
    else if (strcmp(name, "speak")       == 0 ||
             strcmp(name, "talking")     == 0)      { p.mouth_shape = 3; p.mouth_h = 10; }
    else                                              p.mouth_shape = 0;
}

void draw_eye(int cx, int cy, const FaceParams &p, bool closed,
              uint16_t eye_c, uint16_t bg_c) {
    if (closed) {
        fb_hline(cx - p.eye_r, cy,     p.eye_r * 2, eye_c);
        fb_hline(cx - p.eye_r, cy + 1, p.eye_r * 2, eye_c);
        return;
    }
    fb_fill_circle(cx, cy, p.eye_r,     eye_c);
    fb_fill_circle(cx, cy, p.eye_r - 4, bg_c);
    fb_fill_circle(cx + p.pupil_dx, cy + p.pupil_dy, p.eye_r - 12, eye_c);
    fb_fill_circle(cx + p.pupil_dx - 6, cy + p.pupil_dy - 6, 3, rgb565(255, 255, 255));
}

void draw_mouth(const FaceParams &p, uint16_t c, uint16_t bg) {
    const int cx = W / 2;
    switch (p.mouth_shape) {
    case 0:
        fb_fill_round_rect(cx - p.mouth_w / 2, p.mouth_y - 2, p.mouth_w, 5, 2, c);
        break;
    case 1: {
        for (int t = 0; t <= 12; ++t) {
            float a = (float)t / 12.0f;
            int x = cx - p.mouth_w / 2 + (int)(a * p.mouth_w);
            int y = p.mouth_y + (int)(sinf(a * 3.14159f) * p.mouth_h);
            fb_fill_circle(x, y, 3, c);
        }
        break;
    }
    case 2: {
        for (int t = 0; t <= 12; ++t) {
            float a = (float)t / 12.0f;
            int x = cx - p.mouth_w / 2 + (int)(a * p.mouth_w);
            int y = p.mouth_y - (int)(sinf(a * 3.14159f) * p.mouth_h);
            fb_fill_circle(x, y, 3, c);
        }
        break;
    }
    case 3:
        fb_fill_circle(cx, p.mouth_y, p.mouth_h,     c);
        fb_fill_circle(cx, p.mouth_y, p.mouth_h - 4, bg);
        break;
    case 4:
        fb_fill_circle(cx, p.mouth_y, 5, c);
        break;
    case 5:
        fb_line(cx - 16, p.mouth_y - 6, cx,       p.mouth_y + 6, c);
        fb_line(cx,      p.mouth_y + 6, cx + 16,  p.mouth_y - 6, c);
        fb_line(cx - 16, p.mouth_y - 5, cx,       p.mouth_y + 7, c);
        fb_line(cx,      p.mouth_y + 7, cx + 16,  p.mouth_y - 5, c);
        break;
    }
    if (p.show_tongue) {
        fb_fill_round_rect(cx - 10, p.mouth_y + 8, 20, 14, 5, rgb565(0xFF, 0x55, 0x77));
    }
}

void draw_brows(const FaceParams &p, uint16_t c) {
    const int cx     = W / 2;
    const int leftE  = cx - p.eye_dx;
    const int rightE = cx + p.eye_dx;
    const int browY  = p.eye_y - p.eye_r - 10;
    if (p.brow_angle < 0) {
        fb_fill_triangle(leftE - 16, browY,
                         leftE + 16, browY + 12,
                         leftE + 16, browY - 2, c);
        fb_fill_triangle(rightE + 16, browY,
                         rightE - 16, browY + 12,
                         rightE - 16, browY - 2, c);
    } else if (p.brow_angle > 0) {
        fb_fill_triangle(leftE - 16, browY + 12,
                         leftE + 16, browY,
                         leftE - 16, browY + 14, c);
        fb_fill_triangle(rightE + 16, browY + 12,
                         rightE - 16, browY,
                         rightE + 16, browY + 14, c);
    } else {
        fb_fill_round_rect(leftE  - 16, browY, 32, 4, 2, c);
        fb_fill_round_rect(rightE - 16, browY, 32, 4, 2, c);
    }
}

void draw_decorations(const FaceParams &p) {
    const int cx = W / 2;
    if (p.show_z) {
        // small "z Z" in top-right corner, using our 5x7 font (no Chinese).
        fb_glyph(W - 40, 30, 'Z', 4, rgb565(0xCC, 0xCC, 0xFF));
        fb_glyph(W - 25, 60, 'Z', 3, rgb565(0xCC, 0xCC, 0xFF));
    }
    if (p.show_heart) {
        const int hx = 50, hy = 50;
        uint16_t pink = rgb565(0xFF, 0x55, 0x77);
        fb_fill_circle(hx - 6, hy, 8, pink);
        fb_fill_circle(hx + 6, hy, 8, pink);
        fb_fill_triangle(hx - 13, hy + 3, hx + 13, hy + 3, hx, hy + 18, pink);
    }
    if (p.show_sweat) {
        uint16_t blue = rgb565(0x66, 0xBB, 0xEE);
        fb_fill_circle(cx + 70, p.eye_y - 20, 5, blue);
        fb_fill_triangle(cx + 65, p.eye_y - 20,
                         cx + 75, p.eye_y - 20,
                         cx + 70, p.eye_y - 32, blue);
    }
}

// Face state (updated by lcd_face_set, consumed by render_state).
char     g_face_expr[16] = "neutral";
uint32_t g_face_eye      = 0xFFFFFF;
uint32_t g_face_mouth    = 0xFFFFFF;
uint32_t g_face_bg       = 0x000000;

void render_face() {
    uint16_t bg     = rgb24_to_565(g_face_bg);
    uint16_t eyeC   = rgb24_to_565(g_face_eye);
    uint16_t mouthC = rgb24_to_565(g_face_mouth);
    fb_fill(bg);
    FaceParams p;
    face_preset(g_face_expr, p);
    draw_brows(p, eyeC);
    const int cx = W / 2;
    draw_eye(cx - p.eye_dx, p.eye_y, p, p.left_closed,  eyeC, bg);
    draw_eye(cx + p.eye_dx, p.eye_y, p, p.right_closed, eyeC, bg);
    draw_mouth(p, mouthC, bg);
    draw_decorations(p);
}

void render_state(lcd_state_t s) {
    if (s == LCD_STATE_FACE) { render_face(); return; }
    Style sty = style_for(s);
    fb_fill(rgb565(sty.r, sty.g, sty.b));
    // Slight border so transitions are visible
    uint16_t white = rgb565(255, 255, 255);
    for (int i = 0; i < 4; ++i) {
        for (int x = 0; x < W; ++x) { g_fb[i * W + x] = white; g_fb[(H - 1 - i) * W + x] = white; }
        for (int y = 0; y < H; ++y) { g_fb[y * W + i] = white; g_fb[y * W + W - 1 - i] = white; }
    }
    // Status word — big scale (8) centered around y=65 so we have room
    // for an elapsed-seconds counter underneath when state == THINKING.
    int label_y = (s == LCD_STATE_THINKING && g_think_elapsed >= 0) ? 55 : 95;
    fb_text_centered(sty.label, label_y, 8, white);
    if (s == LCD_STATE_THINKING && g_think_elapsed >= 0) {
        char buf[12];
        snprintf(buf, sizeof(buf), "%dS", g_think_elapsed);
        fb_text_centered(buf, 170, 5, white);
    }
}

}  // namespace

esp_err_t lcd_init(void) {
    g_fb = (uint16_t *)heap_caps_malloc(W * H * sizeof(uint16_t),
                                          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!g_fb) {
        ESP_LOGE(TAG, "OOM framebuffer");
        return ESP_ERR_NO_MEM;
    }

    spi_bus_config_t bus = {};
    bus.mosi_io_num     = PIN_MOSI;
    bus.miso_io_num     = -1;
    bus.sclk_io_num     = PIN_SCLK;
    bus.quadwp_io_num   = -1;
    bus.quadhd_io_num   = -1;
    bus.max_transfer_sz = W * H * 2 + 16;
    esp_err_t err = spi_bus_initialize(SPI_HOST_LCD, &bus, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) { ESP_LOGE(TAG, "spi_bus_initialize: %s", esp_err_to_name(err)); return err; }

    esp_lcd_panel_io_spi_config_t io_cfg = {};
    io_cfg.cs_gpio_num     = PIN_CS;
    io_cfg.dc_gpio_num     = PIN_DC;
    io_cfg.spi_mode        = 2;
    io_cfg.pclk_hz         = 40 * 1000 * 1000;
    io_cfg.trans_queue_depth = 4;   // bands are sent serially, queue=4 is plenty
    io_cfg.lcd_cmd_bits    = 8;
    io_cfg.lcd_param_bits  = 8;
    io_cfg.on_color_trans_done = on_color_trans_done;
    err = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI_HOST_LCD, &io_cfg, &g_io);
    if (err != ESP_OK) { ESP_LOGE(TAG, "panel_io_spi: %s", esp_err_to_name(err)); return err; }

    g_trans_sem = xSemaphoreCreateBinary();
    if (!g_trans_sem) { ESP_LOGE(TAG, "OOM trans sem"); return ESP_ERR_NO_MEM; }
    xSemaphoreGive(g_trans_sem);  // first paint can proceed immediately

    esp_lcd_panel_dev_config_t panel_cfg = {};
    panel_cfg.reset_gpio_num = -1;  // AW9523 handles reset; we did the init seq there
    panel_cfg.rgb_ele_order  = LCD_RGB_ELEMENT_ORDER_BGR;
    panel_cfg.bits_per_pixel = 16;
    err = esp_lcd_new_panel_ili9341(g_io, &panel_cfg, &g_panel);
    if (err != ESP_OK) { ESP_LOGE(TAG, "new_panel_ili9341: %s", esp_err_to_name(err)); return err; }

    esp_lcd_panel_reset(g_panel);
    esp_lcd_panel_init(g_panel);
    esp_lcd_panel_invert_color(g_panel, true);
    esp_lcd_panel_swap_xy(g_panel, false);
    esp_lcd_panel_mirror(g_panel, false, false);
    esp_lcd_panel_disp_on_off(g_panel, true);

    render_state(LCD_STATE_BOOT);
    err = draw_fb_banded();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "first banded paint: %s", esp_err_to_name(err));
    }
    ESP_LOGI(TAG, "lcd ready (%dx%d)", W, H);
    return ESP_OK;
}

void lcd_set_think_elapsed(int seconds) {
    if (!g_panel) return;
    if (g_state != LCD_STATE_THINKING) {
        g_think_elapsed = -1;
        return;
    }
    if (seconds == g_think_elapsed) return;
    g_think_elapsed = seconds;
    render_state(g_state);
    esp_err_t err = draw_fb_banded();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "think tick %ds banded paint: %s", seconds, esp_err_to_name(err));
    }
}

void lcd_set_state(lcd_state_t s) {
    if (!g_panel) return;
    // Any explicit state change supersedes a pending auto-IDLE — without
    // this guard the timer fires later and overwrites a fresh LISTEN/THINK.
    if (g_idle_timer && s != LCD_STATE_IDLE) {
        esp_timer_stop(g_idle_timer);
    }
    if (s == g_state) return;
    g_state = s;
    if (s != LCD_STATE_THINKING) g_think_elapsed = -1;
    render_state(s);
    esp_err_t err = draw_fb_banded();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "state %d banded paint: %s", (int)s, esp_err_to_name(err));
    }
    ESP_LOGI(TAG, "state → %d", (int)s);
}

void lcd_face_set(const char *expression, uint32_t eye_rgb,
                  uint32_t mouth_rgb, uint32_t bg_rgb) {
    if (!g_panel) return;
    if (expression) {
        strncpy(g_face_expr, expression, sizeof(g_face_expr) - 1);
        g_face_expr[sizeof(g_face_expr) - 1] = 0;
    }
    g_face_eye   = eye_rgb;
    g_face_mouth = mouth_rgb;
    g_face_bg    = bg_rgb;
    // Cancel any pending auto-IDLE — a face is an explicit visual state.
    if (g_idle_timer) esp_timer_stop(g_idle_timer);
    // Force a repaint even if we were already in LCD_STATE_FACE (the
    // expression / colours may have changed).
    g_state = LCD_STATE_FACE;
    render_face();
    esp_err_t err = draw_fb_banded();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "face paint: %s", esp_err_to_name(err));
    }
    ESP_LOGI(TAG, "face → %s eye=#%06lx mouth=#%06lx bg=#%06lx",
             g_face_expr,
             (unsigned long)(eye_rgb & 0xFFFFFFul),
             (unsigned long)(mouth_rgb & 0xFFFFFFul),
             (unsigned long)(bg_rgb & 0xFFFFFFul));
}

namespace {
void idle_timer_cb(void *) {
    // Only auto-flip from "passive" states (something just ended, we're
    // waiting for the next thing). Active states like LISTENING / THINKING
    // mean bridge is mid-operation; we shouldn't preempt them.
    switch (g_state) {
    case LCD_STATE_SPEAKING:   // post-TTS settle
    case LCD_STATE_HEARD:      // mic_stop with no follow-up
    case LCD_STATE_ASR:        // server-side ASR took too long
    case LCD_STATE_ASR_ERR:    // ASR failed, transient error display
        lcd_set_state(LCD_STATE_IDLE);
        break;
    default:
        break;
    }
}
}  // namespace

void lcd_arm_idle_in(uint32_t delay_ms) {
    if (!g_panel) return;
    if (!g_idle_timer) {
        esp_timer_create_args_t args = {};
        args.callback = idle_timer_cb;
        args.name     = "lcd_idle";
        if (esp_timer_create(&args, &g_idle_timer) != ESP_OK) {
            ESP_LOGW(TAG, "esp_timer_create(lcd_idle) failed");
            return;
        }
    }
    esp_timer_stop(g_idle_timer);  // restart if already pending
    esp_timer_start_once(g_idle_timer, (uint64_t)delay_ms * 1000);
}

// CoreS3 LCD — ILI9342C via SPI3, raw esp_lcd panel.
//
// Big colored background + scaled 5×7 ASCII bitmap font for the status
// word. PSRAM-backed framebuffer (320×240×2 = 153.6 KB) so we can paint
// the whole screen in one bus transaction and avoid tearing.

#include "lcd.h"

#include <stdio.h>
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
    }
    return { 0, 0, 0, "" };
}

int g_think_elapsed = -1;   // seconds; <0 = hide

void render_state(lcd_state_t s) {
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

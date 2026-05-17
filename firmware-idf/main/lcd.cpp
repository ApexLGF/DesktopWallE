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

esp_lcd_panel_handle_t g_panel = nullptr;
uint16_t              *g_fb    = nullptr;     // PSRAM framebuffer, RGB565 BE
lcd_state_t            g_state = LCD_STATE_BOOT;

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
    }
    return { 0, 0, 0, "" };
}

void render_state(lcd_state_t s) {
    Style sty = style_for(s);
    fb_fill(rgb565(sty.r, sty.g, sty.b));
    // Slight border so transitions are visible
    uint16_t white = rgb565(255, 255, 255);
    for (int i = 0; i < 4; ++i) {
        for (int x = 0; x < W; ++x) { g_fb[i * W + x] = white; g_fb[(H - 1 - i) * W + x] = white; }
        for (int y = 0; y < H; ++y) { g_fb[y * W + i] = white; g_fb[y * W + W - 1 - i] = white; }
    }
    // Status word — big scale (8) centered roughly mid-screen.
    fb_text_centered(sty.label, 95, 8, white);
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

    esp_lcd_panel_io_handle_t io = nullptr;
    esp_lcd_panel_io_spi_config_t io_cfg = {};
    io_cfg.cs_gpio_num     = PIN_CS;
    io_cfg.dc_gpio_num     = PIN_DC;
    io_cfg.spi_mode        = 2;
    io_cfg.pclk_hz         = 40 * 1000 * 1000;
    io_cfg.trans_queue_depth = 4;
    io_cfg.lcd_cmd_bits    = 8;
    io_cfg.lcd_param_bits  = 8;
    err = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI_HOST_LCD, &io_cfg, &io);
    if (err != ESP_OK) { ESP_LOGE(TAG, "panel_io_spi: %s", esp_err_to_name(err)); return err; }

    esp_lcd_panel_dev_config_t panel_cfg = {};
    panel_cfg.reset_gpio_num = -1;  // AW9523 handles reset; we did the init seq there
    panel_cfg.rgb_ele_order  = LCD_RGB_ELEMENT_ORDER_BGR;
    panel_cfg.bits_per_pixel = 16;
    err = esp_lcd_new_panel_ili9341(io, &panel_cfg, &g_panel);
    if (err != ESP_OK) { ESP_LOGE(TAG, "new_panel_ili9341: %s", esp_err_to_name(err)); return err; }

    esp_lcd_panel_reset(g_panel);
    esp_lcd_panel_init(g_panel);
    esp_lcd_panel_invert_color(g_panel, true);
    esp_lcd_panel_swap_xy(g_panel, false);
    esp_lcd_panel_mirror(g_panel, false, false);
    esp_lcd_panel_disp_on_off(g_panel, true);

    render_state(LCD_STATE_BOOT);
    esp_lcd_panel_draw_bitmap(g_panel, 0, 0, W, H, g_fb);
    ESP_LOGI(TAG, "lcd ready (%dx%d)", W, H);
    return ESP_OK;
}

void lcd_set_state(lcd_state_t s) {
    if (!g_panel) return;
    if (s == g_state) return;
    g_state = s;
    render_state(s);
    // Throttle: 320*240*2 = 153.6 KB at 40 MHz SPI ≈ 32 ms. If callers
    // hammer state changes faster than that the panel_io queue fills
    // and the next draw_bitmap fails with EAGAIN. 50 ms is a safe pad.
    static int64_t last_paint_us = 0;
    int64_t now_us = esp_timer_get_time();
    int64_t since  = now_us - last_paint_us;
    if (since < 50000) vTaskDelay(pdMS_TO_TICKS((50000 - since) / 1000));
    last_paint_us = esp_timer_get_time();
    esp_err_t err = esp_lcd_panel_draw_bitmap(g_panel, 0, 0, W, H, g_fb);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "draw_bitmap %s — will retry on next state change",
                 esp_err_to_name(err));
    }
    ESP_LOGI(TAG, "state → %d", (int)s);
}

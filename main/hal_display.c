#include "hal_display.h"
#include "caster.h"
#include "driver/spi_master.h"
#include "driver/ledc.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_ili9341.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "display";

/* Caster pacing: full frames are ~150 KB raw, the wire does ~100 KB/s, so
 * uncapped flush_all() calls queue up and stall the UI thread in
 * uart_write_bytes. Cap the caster at ~15 fps, skip (don't queue) while the
 * wire is busy, and re-send / heal periodically via caster_poll(). The TFT
 * path is never throttled. */
#define CASTER_MIN_US   66000    /* min gap between caster frames */
#define CASTER_HEAL_US  2000000  /* forced full refresh (heals CRC loss) */

static int64_t s_caster_last = 0; /* us of last frame pushed to the wire */
static int s_caster_stale = 0;    /* a frame was skipped: refresh owed */

static uint16_t *s_top = NULL;
static uint16_t *s_bot = NULL;
static hacku_fb_t s_fb;
static esp_lcd_panel_handle_t s_panel = NULL;
static int s_has_tft = 0;

hacku_fb_t *hacku_display_fb(void) { return &s_fb; }
int hacku_display_has_tft(void) { return s_has_tft; }

uint16_t *hacku_display_row(int y) {
    if ((unsigned)y >= (unsigned)HACKU_DISP_H)
        return s_top;
    return y < FB_HALF_H ? s_top + (size_t)y * HACKU_DISP_W
                         : s_bot + (size_t)(y - FB_HALF_H) * HACKU_DISP_W;
}

/* Try to init the ILI9341. Returns 1 when a real panel answers (RDDID). */
static int tft_probe(void) {
    spi_bus_config_t buscfg = {
        .sclk_io_num     = PIN_TFT_SCK,
        .mosi_io_num     = PIN_TFT_MOSI,
        .miso_io_num     = PIN_TFT_MISO,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = HACKU_DISP_W * 16 * sizeof(uint16_t),
    };
    if (spi_bus_initialize(TFT_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO) != ESP_OK) {
        ESP_LOGW(TAG, "SPI bus init failed");
        return 0;
    }

    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num          = PIN_TFT_CS,
        .dc_gpio_num          = PIN_TFT_DC,
        .spi_mode             = 0,
        .pclk_hz              = TFT_SPI_HZ,
        .trans_queue_depth    = 10,
        .lcd_cmd_bits         = 8,
        .lcd_param_bits       = 8,
    };
    if (esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)TFT_SPI_HOST,
                                 &io_cfg, &io) != ESP_OK) {
        ESP_LOGW(TAG, "panel IO create failed");
        spi_bus_free(TFT_SPI_HOST);
        return 0;
    }

    esp_lcd_panel_dev_config_t pcfg = {
        .reset_gpio_num = PIN_TFT_RST,
        .rgb_ele_order  = LCD_RGB_ELEMENT_ORDER_BGR,
        .bits_per_pixel = 16,
    };
    if (esp_lcd_new_panel_ili9341(io, &pcfg, &s_panel) != ESP_OK) {
        ESP_LOGW(TAG, "panel create failed");
        esp_lcd_panel_io_del(io);
        spi_bus_free(TFT_SPI_HOST);
        return 0;
    }
    esp_lcd_panel_reset(s_panel);
    esp_lcd_panel_init(s_panel);

    /* RDDID (0x04): expect [dummy, 0x93, 0x41] on genuine ILI9341. */
    uint8_t id[3] = {0};
    if (esp_lcd_panel_io_rx_param(io, 0x04, id, 3) != ESP_OK ||
        id[1] != 0x93 || id[2] != 0x41) {
        ESP_LOGW(TAG, "no ILI9341 (id %02x %02x %02x), headless mode",
                 id[0], id[1], id[2]);
        esp_lcd_panel_del(s_panel);
        s_panel = NULL;
        esp_lcd_panel_io_del(io);
        spi_bus_free(TFT_SPI_HOST);
        return 0;
    }

    esp_lcd_panel_disp_on_off(s_panel, true);
    ESP_LOGI(TAG, "ILI9341 found (id %02x %02x %02x)", id[0], id[1], id[2]);
    return 1;
}

int hacku_display_init(void) {
    /* Two half-framebuffers: each fits fragmented DRAM where one
     * full 150 KB block no longer does (WiFi/BT static eats into it). */
    size_t half = (size_t)HACKU_DISP_W * FB_HALF_H * 2;
    s_top = heap_caps_malloc(half, MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    s_bot = heap_caps_malloc(half, MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    if (!s_top || !s_bot) {
        ESP_LOGE(TAG, "framebuffer alloc failed");
        return -1;
    }
    s_fb.top = s_top;
    s_fb.bot = s_bot;
    memset(s_top, 0, half);
    memset(s_bot, 0, half);

#ifdef CONFIG_HACKU_FORCE_CASTER
    ESP_LOGW(TAG, "HACKU_FORCE_CASTER: skipping TFT probe");
    s_has_tft = 0;
#else
    s_has_tft = tft_probe();
#endif
    s_caster_last = esp_timer_get_time();
    return s_has_tft;
}

void hacku_display_flush(int x, int y, int w, int h) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > HACKU_DISP_W) w = HACKU_DISP_W - x;
    if (y + h > HACKU_DISP_H) h = HACKU_DISP_H - y;
    if (w <= 0 || h <= 0 || !s_top)
        return;

    /* Caster: each bank is row-contiguous, so split the rect at the bank
     * boundary (at most 2 multi-row frames). Stride stays full width.
     * Paced: skip (mark stale) when outrunning the wire or while the
     * previous frame still drains — the UI thread must never block. */
    if (caster_is_active()) {
        int64_t now = esp_timer_get_time();
        if (now - s_caster_last < CASTER_MIN_US || caster_tx_busy()) {
            s_caster_stale = 1;
        } else {
            int y1 = y + h;
            int mid = FB_HALF_H;
            if (y < mid) {
                int h0 = (y1 < mid ? y1 : mid) - y;
                caster_send_rect(x, y, w, h0, hacku_display_row(y) + x,
                                 HACKU_DISP_W);
            }
            if (y1 > mid) {
                int y0 = y > mid ? y : mid;
                caster_send_rect(x, y0, w, y1 - y0, hacku_display_row(y0) + x,
                                 HACKU_DISP_W);
            }
            s_caster_last = now;
            s_caster_stale = 0;
        }
    }

    /* TFT needs contiguous rows: push line by line through a small buffer. */
    if (s_has_tft && s_panel) {
        static uint16_t line[HACKU_DISP_W];
        for (int r = 0; r < h; r++) {
            memcpy(line, hacku_display_row(y + r) + x, (size_t)w * 2);
            esp_lcd_panel_draw_bitmap(s_panel, x, y + r, x + w, y + r + 1, line);
        }
    }
}

void hacku_display_flush_all(void) {
    hacku_display_flush(0, 0, HACKU_DISP_W, HACKU_DISP_H);
}

/* Catch-up + self-heal for the caster. Call every main-loop iteration:
 * pushes a full refresh when a frame was skipped, or periodically so a
 * viewer region lost to line noise converges back. TFT untouched. */
void hacku_display_caster_poll(void) {
    if (!caster_is_active())
        return;
    int64_t now = esp_timer_get_time();
    int due = s_caster_stale ? now - s_caster_last >= CASTER_MIN_US
                             : now - s_caster_last >= CASTER_HEAL_US;
    if (!due || caster_tx_busy())
        return;
    caster_send_rect(0, 0, HACKU_DISP_W, FB_HALF_H,
                     hacku_display_row(0), HACKU_DISP_W);
    caster_send_rect(0, FB_HALF_H, HACKU_DISP_W, HACKU_DISP_H - FB_HALF_H,
                     hacku_display_row(FB_HALF_H), HACKU_DISP_W);
    s_caster_last = now;
    s_caster_stale = 0;
}

void hacku_display_set_brightness(int pct) {
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    static int init = 0;
    if (!init) {
        ledc_timer_config_t t = {
            .speed_mode = LEDC_LOW_SPEED_MODE,
            .duty_resolution = LEDC_TIMER_8_BIT,
            .timer_num = LEDC_TIMER_0,
            .freq_hz = 5000,
            .clk_cfg = LEDC_AUTO_CLK,
        };
        ledc_timer_config(&t);
        ledc_channel_config_t c = {
            .gpio_num = PIN_TFT_BL,
            .speed_mode = LEDC_LOW_SPEED_MODE,
            .channel = LEDC_CHANNEL_0,
            .timer_sel = LEDC_TIMER_0,
            .duty = 0,
            .hpoint = 0,
        };
        ledc_channel_config(&c);
        init = 1;
    }
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, pct * 255 / 100);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

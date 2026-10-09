#include "hal_oled.h"
#include "caster.h"
#include "driver/i2c.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_ssd1306.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <string.h>

static const char *TAG = "oled";

/* Caster pacing: mono frames expand to ~16 KB RGB565 on the wire, so keep
 * the same skip-don't-queue policy as the old TFT path. The OLED path is
 * never throttled (1 KB over I2C is cheap). */
#define CASTER_MIN_US   66000    /* min gap between caster frames */
#define CASTER_HEAL_US  2000000  /* forced full refresh (heals CRC loss) */

static int64_t s_caster_last = 0;
static int s_caster_stale = 0;

static oled_fb_t s_fb;
static esp_lcd_panel_handle_t s_panel = NULL;
static int s_has_oled = 0;

/* RGB565 expansion for the caster wire (PKC protocol is RGB565 RLE).
 * .bss, not heap: keeps the contiguous block free for WiFi/BT. Only the
 * requested rect is filled; stride stays full width. */
static uint16_t s_rgb[OLED_W * OLED_H];

oled_fb_t *hal_oled_fb(void) {
    return &s_fb;
}

int hal_oled_has_display(void) {
    return s_has_oled;
}

int hal_oled_get_px(int x, int y) {
    return oled_pget(&s_fb, x, y);
}

/* Probe one I2C address: 1 = ACK, 0 = silence. */
static int i2c_probe(uint8_t addr) {
    return i2c_master_write_to_device(OLED_I2C_PORT, addr, NULL, 0,
                                      pdMS_TO_TICKS(50)) == ESP_OK;
}

static int oled_probe(void) {
    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = PIN_OLED_SDA,
        .scl_io_num = PIN_OLED_SCL,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = OLED_I2C_HZ,
    };
    if (i2c_param_config(OLED_I2C_PORT, &conf) != ESP_OK) {
        ESP_LOGW(TAG, "I2C param config failed");
        return 0;
    }
    if (i2c_driver_install(OLED_I2C_PORT, conf.mode, 0, 0, 0) != ESP_OK) {
        ESP_LOGW(TAG, "I2C install failed");
        return 0;
    }
    uint8_t addr = OLED_ADDR;
    if (!i2c_probe(addr)) {
        /* Try the alternate SSD1306 address before giving up. */
        uint8_t alt = addr == 0x3C ? 0x3D : 0x3C;
        if (i2c_probe(alt)) {
            addr = alt;
        } else {
            ESP_LOGW(TAG, "no OLED at 0x3C/0x3D, caster-only mode");
            return 0;
        }
    }
    ESP_LOGI(TAG, "OLED ACK at 0x%02X", addr);

    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_io_i2c_config_t io_cfg = {
        .dev_addr = addr,
        .scl_speed_hz = OLED_I2C_HZ,
        .control_phase_bytes = 1,
        .dc_bit_offset = 6,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    if (esp_lcd_new_panel_io_i2c(OLED_I2C_PORT, &io_cfg, &io) != ESP_OK) {
        ESP_LOGW(TAG, "panel IO create failed");
        return 0;
    }
    esp_lcd_panel_ssd1306_config_t ssd_cfg = {
        .height = OLED_H,
    };
    esp_lcd_panel_dev_config_t pcfg = {
        .reset_gpio_num = -1,
        .bits_per_pixel = 1,
        .vendor_config = &ssd_cfg,
    };
    if (esp_lcd_new_panel_ssd1306(io, &pcfg, &s_panel) != ESP_OK) {
        ESP_LOGW(TAG, "panel create failed");
        esp_lcd_panel_io_del(io);
        s_panel = NULL;
        return 0;
    }
    esp_lcd_panel_reset(s_panel);
    esp_lcd_panel_init(s_panel);
    esp_lcd_panel_disp_on_off(s_panel, true);
    ESP_LOGI(TAG, "SSD1306 up (%dx%d)", OLED_W, OLED_H);
    return 1;
}

int hal_oled_init(void) {
    memset(&s_fb, 0, sizeof(s_fb));
    s_has_oled = oled_probe();
    s_caster_last = esp_timer_get_time();
    return s_has_oled;
}

static void push_oled(void) {
    if (!s_has_oled || !s_panel) {
        return;
    }
#ifdef CONFIG_HACKU_OLED_SH1106
    /* SH1106 GDDRAM is 132 wide: shift the 128-wide image by 2 columns. */
    esp_lcd_panel_draw_bitmap(s_panel, 2, 0, 2 + OLED_W, OLED_H, s_fb.pages);
#else
    esp_lcd_panel_draw_bitmap(s_panel, 0, 0, OLED_W, OLED_H, s_fb.pages);
#endif
}

void hal_oled_flush(int x, int y, int w, int h) {
    if (x < 0) {
        w += x;
        x = 0;
    }
    if (y < 0) {
        h += y;
        y = 0;
    }
    if (x + w > OLED_W) {
        w = OLED_W - x;
    }
    if (y + h > OLED_H) {
        h = OLED_H - y;
    }
    if (w <= 0 || h <= 0) {
        return;
    }
    push_oled();

    /* Caster mirror: expand the mono rect to RGB565 (white/black) and emit
     * it through the unchanged PKC wire protocol. Paced: skip (mark stale)
     * when outrunning the wire — the UI thread must never block. */
    if (caster_is_active()) {
        int64_t now = esp_timer_get_time();
        if (now - s_caster_last < CASTER_MIN_US || caster_tx_busy()) {
            s_caster_stale = 1;
        } else {
            for (int r = 0; r < h; r++) {
                for (int i = 0; i < w; i++)
                    s_rgb[(size_t)(y + r) * OLED_W + x + i] =
                        oled_pget(&s_fb, x + i, y + r) ? 0xFFFF : 0x0000;
            }
            caster_send_rect(x, y, w, h, &s_rgb[(size_t)y * OLED_W + x],
                             OLED_W);
            s_caster_last = now;
            s_caster_stale = 0;
        }
    }
}

void hal_oled_flush_all(void) {
    hal_oled_flush(0, 0, OLED_W, OLED_H);
}

void hal_oled_caster_poll(void) {
    if (!caster_is_active()) {
        return;
    }
    int64_t now = esp_timer_get_time();
    int due = s_caster_stale ? now - s_caster_last >= CASTER_MIN_US
                             : now - s_caster_last >= CASTER_HEAL_US;
    if (!due || caster_tx_busy()) {
        return;
    }
    for (int yy = 0; yy < OLED_H; yy++) {
        for (int xx = 0; xx < OLED_W; xx++)
            s_rgb[(size_t)yy * OLED_W + xx] =
                oled_pget(&s_fb, xx, yy) ? 0xFFFF : 0x0000;
    }
    caster_send_rect(0, 0, OLED_W, OLED_H, s_rgb, OLED_W);
    s_caster_last = now;
    s_caster_stale = 0;
}

void hal_oled_set_contrast(int pct) {
    /* SSD1306 has no backlight; contrast needs vendor commands the generic
     * esp_lcd API does not expose. Kept as a no-op for API compatibility. */
    (void)pct;
    if (!s_has_oled || !s_panel) {
        return;
    }
}

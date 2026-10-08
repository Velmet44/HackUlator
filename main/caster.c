#include "caster.h"
#include "app_config.h"
#include "driver/uart.h"
#include "esp_log.h"

static const char *TAG = "caster";

static TaskHandle_t  s_task       = NULL;
static QueueHandle_t s_rx_q       = NULL;
static volatile int  s_active     = 0;
static volatile int  s_uart_ready  = 0;

int caster_is_active(void) { return s_active; }
QueueHandle_t caster_get_rx_queue(void) { return s_rx_q; }

static uint8_t crc8(const uint8_t *buf, int len) {
    uint8_t crc = 0;
    for (int i = 0; i < len; i++) {
        crc ^= buf[i];
        for (int b = 0; b < 8; b++)
            crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x07) : (uint8_t)(crc << 1);
    }
    return crc;
}

/* uart_write_bytes blocks until the chunk fits the TX ring; the ring drains
 * at wire speed (~125 KB/s @ 1 Mbaud), so a busy UI throttles to the wire. */
static void uart_tx(const uint8_t *data, size_t len) {
    while (len > 0) {
        size_t n = len > 1024 ? 1024 : len;
        uart_write_bytes(UART_NUM_0, data, n);
        data += n;
        len  -= n;
    }
}

/* One row: [enc_len:u16] + payload. enc_len == w*2 -> raw RGB565,
 * otherwise RLE runs of [count:u16][pixel:u16]. Raw wins ties. */
static void send_row(int w, const uint16_t *pix) {
    static uint16_t enc[HACKU_DISP_W * 2]; /* [count,px] pairs, .bss not stack */
    int pairs = 0, i = 0;

    while (i < w) {
        uint16_t v = pix[i];
        int j = i + 1;
        while (j < w && pix[j] == v)
            j++;
        if (pairs + 1 >= w)
            break; /* RLE can never beat raw from here */
        enc[pairs * 2]     = (uint16_t)(j - i);
        enc[pairs * 2 + 1] = v;
        pairs++;
        i = j;
    }

    uint16_t elen;
    if (pairs == 0 || pairs * 4 >= w * 2) {
        elen = (uint16_t)(w * 2);
        uart_tx((const uint8_t *)&elen, 2);
        uart_tx((const uint8_t *)pix, (size_t)w * 2);
    } else {
        elen = (uint16_t)(pairs * 4);
        uart_tx((const uint8_t *)&elen, 2);
        uart_tx((const uint8_t *)enc, elen);
    }
}

void caster_send_rect(int x, int y, int w, int h, const uint16_t *pixels,
                      int stride) {
    if (!s_active || !s_uart_ready || w <= 0 || h <= 0 || pixels == NULL)
        return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > HACKU_DISP_W) w = HACKU_DISP_W - x;
    if (y + h > HACKU_DISP_H) h = HACKU_DISP_H - y;
    if (w <= 0 || h <= 0)
        return;

    uint8_t hdr[15];
    hdr[0] = CASTER_SYNC0; hdr[1] = CASTER_SYNC1; hdr[2] = CASTER_SYNC2;
    hdr[3] = CASTER_TAG0;  hdr[4] = CASTER_TAG1;  hdr[5] = CASTER_TAG2;
    hdr[6] = x & 0xFF;        hdr[7]  = (x >> 8) & 0xFF;
    hdr[8] = y & 0xFF;        hdr[9]  = (y >> 8) & 0xFF;
    hdr[10] = w & 0xFF;       hdr[11] = (w >> 8) & 0xFF;
    hdr[12] = h & 0xFF;       hdr[13] = (h >> 8) & 0xFF;
    hdr[14] = crc8(&hdr[6], 8);

    uart_tx(hdr, sizeof(hdr));
    for (int row = 0; row < h; row++)
        send_row(w, pixels + (size_t)row * stride);
}

/* Zero-timeout TX-done poll: 1 while the previous frame is still on the
 * wire. Lets the display HAL skip instead of queueing behind it. */
int caster_tx_busy(void) {
    if (!s_active || !s_uart_ready)
        return 0;
    return uart_wait_tx_done(UART_NUM_0, 0) != ESP_OK;
}

static void caster_task(void *arg) {
    (void)arg;
    uart_config_t cfg = {
        .baud_rate  = CASTER_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    /* RX ring for remote key bytes, TX ring for the rect stream. Small
     * rings: uart_write_bytes blocks, so a busy UI throttles to the wire.
     * (Big rings would steal the contiguous heap WiFi/BT need: RX holds a
     * few key bytes, TX just smooths bursts — 2.5 KB saved vs stock.) */
    uart_driver_install(UART_NUM_0, 512, 2048, 0, NULL, 0);
    uart_param_config(UART_NUM_0, &cfg);
    uart_set_pin(UART_NUM_0, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE,
                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);

    if (s_rx_q == NULL)
        s_rx_q = xQueueCreate(128, 1);
    s_uart_ready = 1;

    uint8_t buf[64];
    for (;;) {
        size_t avail = 0;
        uart_get_buffered_data_len(UART_NUM_0, &avail);
        while (avail > 0) {
            size_t n = avail > sizeof(buf) ? sizeof(buf) : avail;
            int rd = uart_read_bytes(UART_NUM_0, buf, n, 0);
            if (rd <= 0)
                break;
            for (int i = 0; i < rd; i++)
                xQueueSend(s_rx_q, &buf[i], 0);
            avail -= (size_t)rd;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void caster_start(void) {
    if (s_task)
        return;
    s_active = 1;
    /* Logs would corrupt the rect stream mid-frame: silence for the session. */
    esp_log_level_set("*", ESP_LOG_NONE);
    xTaskCreatePinnedToCore(caster_task, "caster", 4096, NULL, 4, &s_task, 0);
    ESP_LOGI(TAG, "caster on UART0 @%d baud", CASTER_BAUD);
}

void caster_stop(void) {
    s_active = 0;
    s_uart_ready = 0;
    if (s_task) {
        vTaskDelete(s_task);
        s_task = NULL;
    }
    esp_log_level_set("*", ESP_LOG_INFO);
}

#include "svc_nrf.h"
#include "app_config.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_rom_sys.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "nrf";

/* nRF24L01+ max SPI is 10 MHz. These modules are almost always on dupont
 * jumper wire at this project's pin pitch, and SCK is the first thing to
 * break on it, so run one step under and keep the whole sweep affordable:
 * 8 MHz is still ~16x faster than the ~190 us settle dominates. */
#define NRF_SPI_HZ  (8 * 1000 * 1000)

/* GPIO18/19/23 are the VSPI (SPI3) IO_MUX pins on the classic ESP32, so
 * this runs at native speed instead of being dragged through the GPIO
 * matrix. GPIO5 is SPI3's own CS0 and is NOT used: CSN here is GPIO14,
 * which is not an IO_MUX pin for this peripheral, so the driver's hardware
 * CS cannot reach it. CSN is therefore driven as a plain GPIO and gated by
 * hand around every transaction (cs_low/cs_high below). That also lets the
 * code re-assert it, which is what keeps a pressed OK button from latching
 * the radio selected - see app_config.h for why that pin is shared. */
static const spi_bus_config_t s_bus = {
    .mosi_io_num     = PIN_NRF_MOSI,
    .miso_io_num     = PIN_NRF_MISO,
    .sclk_io_num     = PIN_NRF_SCK,
    .quadwp_io_num   = -1,
    .quadhd_io_num   = -1,
    .max_transfer_sz = 8,
};

/* Mode 0 (CPOL=0, CPHA=0), MSB first, no CS pin of our own. */
static const spi_device_interface_config_t s_devcfg = {
    .clock_speed_hz = NRF_SPI_HZ,
    .mode           = 0,
    .spics_io_num   = -1,
    .queue_size     = 1,
};

static spi_device_handle_t s_dev;
static int s_present;
static int s_spi_err;
static int s_rate = NRF_RATE_1M;
static int s_samples = 8;
static int s_peak_hold = 1;
static int s_cursor;
static uint32_t s_sweeps;

/* Live hit counts and the decaying per-channel maximum. Static: the sweep
 * is the only writer and the UI only reads, so there is no locking here and
 * no allocation on a board whose largest-free-block is the binding metric. */
static uint8_t s_level[NRF_CH_COUNT];
static uint8_t s_peak[NRF_CH_COUNT];

/* ---- nRF24L01+ register map ---- */
#define REG_CONFIG      0x00
#define REG_EN_AA       0x01
#define REG_EN_RXADDR   0x02
#define REG_SETUP_AW    0x03
#define REG_SETUP_RETR  0x04
#define REG_RF_CH       0x05
#define REG_RF_SETUP    0x06
#define REG_STATUS      0x07
#define REG_RPD         0x09   /* bit 0 = received power detector */
#define REG_RX_ADDR_P0  0x0A
#define REG_RX_PW_P0    0x11

/* CONFIG bits. Note EN_RXADDR is NOT in here - it is the separate register
 * 0x02, one bit per pipe. Bits 6..5..4 of CONFIG are the IRQ masks. */
#define CFG_PRIM_RX     0x01
#define CFG_PWR_UP      0x02
#define CFG_CRCO        0x04
#define CFG_EN_CRC      0x08
#define CFG_MASK_MAX_RT 0x10
#define CFG_MASK_TX_DS  0x20
#define CFG_MASK_RX_DR  0x40

/* RX configuration for a measurement receiver:
 *   PRIM_RX + PWR_UP  stay in RX
 *   EN_CRC/CRCO       0 - the datasheet requires CRC off for RPD
 *   the three masks   1 - IRQ is not wired, so mask it all and stop the
 *                       radio toggling an unconnected pin on every frame
 * EN_RXADDR (0x02) is set separately: RPD only works with a pipe enabled. */
#define CONFIG_RX  (CFG_PRIM_RX | CFG_PWR_UP | \
                    CFG_MASK_MAX_RT | CFG_MASK_TX_DS | CFG_MASK_RX_DR)  /* 0x73 */

/* RF_SETUP bits for the + variant. Bit 0 is LNA_HCURR, which only exists on
 * the P model and is the difference between a receiver that hears the band
 * and one that does not - it is on for every measurement mode here. */
#define RF_LNA_HCURR  0x01
#define RF_DR_HIGH    0x08   /* 1 Mbps = 0, 2 Mbps = 1 */
#define RF_DR_LOW     0x20   /* 250 kbps */

/* Timings, datasheet 6.4 and Table 16.
 *   CE low  -> standby           Tstby2a, ~10 us
 *   CE high -> RPD valid         Tstby2a + Tdelay_AGC = 130 + 40 us
 * Power-down -> standby is up to 5 ms, paid once at init. */
#define CE_LOW_US       10
#define RPD_SETTLE_US   170
#define PWRUP_DELAY_US  5000

static uint8_t rf_setup_for(int rate) {
    switch (rate) {
        case NRF_RATE_250K: return RF_LNA_HCURR | RF_DR_LOW;
        case NRF_RATE_2M:   return RF_LNA_HCURR | RF_DR_HIGH;
        default:            return RF_LNA_HCURR;
    }
}

/* CSN is a plain GPIO (see the note above the bus config). Asserting it
 * high before pulling it low is not redundant: on GPIO14 the OK button
 * holds the line to ground through its own body, and a chip that is still
 * selected sees every following transaction merged into one garbage
 * frame. Re-driving the latch first gives the radio a clean edge back. */
static void cs_low(void) {
    gpio_set_level(PIN_NRF_CSN, 1);
    gpio_set_level(PIN_NRF_CSN, 0);
}

static void cs_high(void) {
    gpio_set_level(PIN_NRF_CSN, 1);
}

/* Every bus access is bracketed by cs_low()/cs_high() HERE, in xfer2() and
 * write_addr(), and nowhere else. The device config asks the driver for no
 * CS pin of its own (spics_io_num = -1) because CSN is not SPI3's IO_MUX CS,
 * so nothing else in the stack will ever select this radio. A helper that
 * forgot the pair would leave it deselected for ever and every read would
 * return the bus idle value 0xFF - which is indistinguishable from an
 * absent chip. Two chokepoints, paired by construction. */
static esp_err_t xfer2(const uint8_t *tx, uint8_t *rx) {
    spi_transaction_t t = {
        .length    = 16,
        .tx_buffer = tx,
        .rx_buffer = rx,
    };
    cs_low();
    esp_err_t e = spi_device_polling_transmit(s_dev, &t);
    cs_high();
    if (e != ESP_OK) {
        s_spi_err = e;
    }
    return e;
}

static uint8_t reg_read(uint8_t addr) {
    uint8_t tx[2];
    uint8_t rx[2] = { 0, 0 };
    tx[0] = (uint8_t)(addr & 0x1F);   /* R_REGISTER | AAAAA */
    tx[1] = 0xFF;                    /* NOP clocks the register out */
    if (xfer2(tx, rx) != ESP_OK) {
        return 0;
    }
    /* The first byte is STATUS, shifted out during the command word; the
     * register value follows. */
    return rx[1];
}

static void reg_write(uint8_t addr, uint8_t val) {
    uint8_t tx[2];
    tx[0] = (uint8_t)(0x20 | (addr & 0x1F));   /* W_REGISTER | AAAAA */
    tx[1] = val;
    (void)xfer2(tx, NULL);
}

static void write_addr(uint8_t addr, const uint8_t *v, int n) {
    uint8_t tx[8];
    tx[0] = (uint8_t)(0x20 | (addr & 0x1F));
    for (int i = 0; i < n && i < 6; i++) {
        tx[i + 1] = v[i];
    }
    spi_transaction_t t = {
        .length    = (size_t)(8 * (n + 1)),
        .tx_buffer = tx,
    };
    cs_low();
    esp_err_t e = spi_device_polling_transmit(s_dev, &t);
    cs_high();
    if (e != ESP_OK) {
        s_spi_err = e;
    }
}

/* Write a value the radio will never hold on its own, read it back, then put
 * the real one back. This is the standard non-destructive presence probe:
 * an absent radio leaves MISO high (reads 0xFF), a wrong SPI mode leaves it
 * low (0x00), and only a real chip returns what was written. */
static int probe(void) {
    uint8_t keep = reg_read(REG_RF_SETUP);
    reg_write(REG_RF_SETUP, 0x27);
    uint8_t got = reg_read(REG_RF_SETUP);
    reg_write(REG_RF_SETUP, keep);
    return (got == 0x27);
}

static void apply_config(void) {
    /* A pipe address is mandatory: RPD does not respond with every pipe
     * disabled, and the datasheet wants a real address on pipe 0. */
    static const uint8_t addr[5] = { 0xC3, 0x3C, 0xE5, 0x71, 0x09 };
    reg_write(REG_EN_AA, 0x00);          /* no auto-ack: nothing to pair with */
    reg_write(REG_EN_RXADDR, 0x01);      /* pipe 0 only */
    reg_write(REG_SETUP_AW, 0x03);       /* 5-byte addresses */
    write_addr(REG_RX_ADDR_P0, addr, 5);
    reg_write(REG_RX_PW_P0, 0x00);       /* zero-length payload: RX FIFO stays empty */
    reg_write(REG_SETUP_RETR, 0x00);     /* never transmitting, so no retransmit */
    reg_write(REG_RF_SETUP, rf_setup_for(s_rate));
    reg_write(REG_CONFIG, CONFIG_RX);
    reg_write(REG_STATUS, 0x07);         /* clear MAX_RT | TX_DS | RX_DR */
    gpio_set_level(PIN_NRF_CE, 1);       /* enter RX */
    esp_rom_delay_us(RPD_SETTLE_US);
}

int svc_nrf_init(void) {
    if (s_dev) {
        return 0;
    }
    s_spi_err = 0;

    /* SPI3 allocates its DMA descriptors from one contiguous internal block.
     * On this board the largest free block is the metric that decides
     * whether a bring-up can work at all, and after a Wi-Fi/BT cycle it is
     * not always there. Fail loudly rather than faulting inside the driver. */
    unsigned largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    if (largest < NRF_MIN_HEAP_BYTES) {
        ESP_LOGE(TAG, "largest block %u < %u, refusing SPI bring-up",
                 (unsigned)largest, (unsigned)NRF_MIN_HEAP_BYTES);
        return NRF_ERR_HEAP;
    }

    gpio_config_t out = {
        .pin_bit_mask = (1ULL << PIN_NRF_CE) | (1ULL << PIN_NRF_CSN),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    if (gpio_config(&out) != ESP_OK) {
        return NRF_ERR_SPI;
    }
    /* Idle state per the datasheet: CE low (standby), CSN high (unselected).
     * CSN first, so the radio is deselected before it is ever driven. */
    gpio_set_level(PIN_NRF_CSN, 1);
    gpio_set_level(PIN_NRF_CE, 0);

    esp_err_t e = spi_bus_initialize(SPI3_HOST, &s_bus, SPI_DMA_CH_AUTO);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_initialize: %s", esp_err_to_name(e));
        s_spi_err = e;
        return NRF_ERR_SPI;
    }
    e = spi_bus_add_device(SPI3_HOST, &s_devcfg, &s_dev);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_add_device: %s", esp_err_to_name(e));
        s_spi_err = e;
        spi_bus_free(SPI3_HOST);
        return NRF_ERR_SPI;
    }

    /* Power up in standby (PWR_UP set, PRIM_RX clear) so the register writes
     * below are legal, then pay the power-up delay once rather than per
     * channel. */
    reg_write(REG_CONFIG, CFG_PWR_UP);
    esp_rom_delay_us(PWRUP_DELAY_US);

    if (!probe()) {
        ESP_LOGW(TAG, "no radio on SPI3 (CSN=GPIO%d, MOSI=%d, MISO=%d, SCK=%d)",
                 PIN_NRF_CSN, PIN_NRF_MOSI, PIN_NRF_MISO, PIN_NRF_SCK);
        s_present = 0;
        return NRF_ERR_ABSENT;
    }

    apply_config();
    s_present = 1;
    svc_nrf_reset();
    ESP_LOGI(TAG, "nRF24L01+ on SPI3 @ %d Hz, 125 ch 2400-2524 MHz, "
                  "%s, %d samples", NRF_SPI_HZ,
             svc_nrf_rate_name(), s_samples);
    return 0;
}

int svc_nrf_present(void) {
    return s_present;
}

void svc_nrf_stop(void) {
    if (s_dev) {
        /* Standby, then let go of the bus and the pins. Both matter: an
         * orphan SPI transaction pointer is the same failure shape as an
         * orphan TX timer in svc_deauth. */
        reg_write(REG_CONFIG, 0x00);
        gpio_set_level(PIN_NRF_CE, 0);
        cs_high();
        spi_bus_remove_device(s_dev);
        spi_bus_free(SPI3_HOST);
        s_dev = NULL;
    }
    s_present = 0;
    s_cursor = 0;
}

void svc_nrf_set_rate(nrf_rate_t r) {
    if (r < 0 || r >= NRF_RATE_COUNT) {
        return;
    }
    s_rate = r;
    if (s_present) {
        reg_write(REG_RF_SETUP, rf_setup_for(s_rate));
        /* The threshold moves with the rate, so the previous trace is not
         * on the same scale any more. Keeping it would draw old samples
         * against a new scale, which is exactly the kind of quiet lie that
         * makes two screenshots mean different things. */
        svc_nrf_reset();
    }
}

nrf_rate_t svc_nrf_rate(void) {
    return (nrf_rate_t)s_rate;
}

const char *svc_nrf_rate_name(void) {
    switch (s_rate) {
        case NRF_RATE_250K: return "250k";
        case NRF_RATE_2M:   return "2M";
        default:            return "1M";
    }
}

void svc_nrf_set_samples(int n) {
    if (n < 1) {
        n = 1;
    }
    if (n > 16) {
        n = 16;
    }
    s_samples = n;
    svc_nrf_reset();
}

int svc_nrf_samples(void) {
    return s_samples;
}

const char *svc_nrf_samples_name(void) {
    if (s_samples <= 4) {
        return "fast";
    }
    if (s_samples >= 12) {
        return "fine";
    }
    return "norm";
}

void svc_nrf_set_peak_hold(int on) {
    s_peak_hold = on ? 1 : 0;
    if (!on) {
        for (int i = 0; i < NRF_CH_COUNT; i++) {
            s_peak[i] = 0;
        }
    }
}

int svc_nrf_peak_hold_on(void) {
    return s_peak_hold;
}

void svc_nrf_reset(void) {
    for (int i = 0; i < NRF_CH_COUNT; i++) {
        s_level[i] = 0;
        s_peak[i] = 0;
    }
    s_cursor = 0;
}

int svc_nrf_step(int max_ch) {
    if (!s_present || !s_dev || max_ch <= 0) {
        return 0;
    }
    int n = 0;
    while (n < max_ch && s_cursor < NRF_CH_COUNT) {
        int ch = s_cursor;
        reg_write(REG_RF_CH, (uint8_t)ch);
        int hits = 0;
        for (int k = 0; k < s_samples; k++) {
            /* Re-enter RX before every sample: RPD is latched, so without
             * this CE cycle all s_samples reads return the same bit and the
             * "level" is always 0 or s_samples. See svc_nrf.h. */
            gpio_set_level(PIN_NRF_CE, 0);
            esp_rom_delay_us(CE_LOW_US);
            gpio_set_level(PIN_NRF_CE, 1);
            esp_rom_delay_us(RPD_SETTLE_US);
            if (reg_read(REG_RPD) & 0x01) {
                hits++;
            }
        }
        s_level[ch] = (uint8_t)hits;
        if (s_peak_hold && hits > s_peak[ch]) {
            s_peak[ch] = (uint8_t)hits;
        }
        s_cursor++;
        n++;
        taskYIELD();
    }
    if (s_cursor >= NRF_CH_COUNT) {
        s_cursor = 0;
        s_sweeps++;
        /* Release the hold one step per sweep. Instant clear would make the
         * envelope flicker uselessly; no decay at all would leave a dead
         * channel lit for ever. */
        if (s_peak_hold) {
            for (int i = 0; i < NRF_CH_COUNT; i++) {
                if (s_peak[i] > 0) {
                    s_peak[i]--;
                }
            }
        }
    }
    return n;
}

int svc_nrf_cursor(void) {
    return s_cursor;
}

uint8_t svc_nrf_level(int ch) {
    if (ch < 0 || ch >= NRF_CH_COUNT) {
        return 0;
    }
    return s_level[ch];
}

uint8_t svc_nrf_peak(int ch) {
    if (ch < 0 || ch >= NRF_CH_COUNT) {
        return 0;
    }
    return s_peak[ch];
}

int svc_nrf_peak_ch(void) {
    int best = -1;
    uint8_t top = 0;
    for (int i = 0; i < NRF_CH_COUNT; i++) {
        if (s_peak[i] > top) {
            top = s_peak[i];
            best = i;
        }
    }
    return best;
}

uint8_t svc_nrf_peak_level(void) {
    int c = svc_nrf_peak_ch();
    return (c < 0) ? 0 : s_peak[c];
}

uint32_t svc_nrf_sweeps(void) {
    return s_sweeps;
}

int svc_nrf_error(void) {
    return s_spi_err;
}
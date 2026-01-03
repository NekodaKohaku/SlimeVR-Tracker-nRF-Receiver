#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/usb/usb_device.h>
#include <zephyr/drivers/gpio.h>
#include <hal/nrf_radio.h>
#include <string.h>

#define ENABLE_HW_CRC        1
#define RSSI_THRESHOLD_DBM  -95
#define LOCK_ON_MS         1500
#define DWELL_MS            200
#define RX_WAIT_US         5000
#define PRINT_BYTES          64

/* 0 = 完全不印重複包；>0 = 同一包連續收到時，每隔 N ms 印一次“累積次數摘要” */
#define FLUSH_SAME_AFTER_MS  500

#define FILTER_IMU_ONLY      0

static const uint8_t target_freqs[] = {
    2, 8,
    34, 36, 40, 42, 44, 46, 48, 50, 52, 54, 56, 58,
    60, 62, 64, 66, 68, 70, 72, 74, 76, 80,
};

#define ADDR_BASE_1       0xD235CF35UL
#define ADDR_PREFIX0      0x23C300C0UL

#define LED0_NODE DT_ALIAS(led0)
static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(LED0_NODE, gpios);

static uint8_t rx_buffer[PRINT_BYTES] __aligned(4);

static int lock_freq = -1;
static int64_t lock_until_ms = 0;

/* ---- Dedup state ---- */
static uint8_t  last_buf[PRINT_BYTES];
static uint8_t  last_len = 0;
static uint32_t last_rep = 0;
static bool     have_last = false;

/* metadata for the run */
static int64_t  last_ts_ms = 0;
static uint8_t  last_freq_off = 0;
static int8_t   last_rssi_dbm = -127;
static uint8_t  last_rxmatch = 0xFF;
static uint8_t  last_crcok = 0xFF;

static int64_t  last_flush_ms = 0;

static inline void radio_disable_clean(void)
{
    NRF_RADIO->EVENTS_DISABLED = 0;
    NRF_RADIO->TASKS_DISABLE = 1;
    while (NRF_RADIO->EVENTS_DISABLED == 0) { /* wait */ }
    NRF_RADIO->EVENTS_DISABLED = 0;
}

/* 回傳：true 表示 RSSIEND 有成功，rssi_dbm 有效；false 表示無效 */
static inline bool radio_sample_rssi_dbm(int8_t *out_rssi_dbm)
{
    NRF_RADIO->EVENTS_RSSIEND = 0;
    NRF_RADIO->TASKS_RSSISTART = 1;

    for (int i = 0; i < 300; i++) {
        if (NRF_RADIO->EVENTS_RSSIEND) {
            NRF_RADIO->EVENTS_RSSIEND = 0;
            if (out_rssi_dbm) *out_rssi_dbm = (int8_t)NRF_RADIO->RSSISAMPLE; /* 通常會是負值 dBm */
            return true;
        }
        k_busy_wait(1);
    }

    /* timeout: RSSI 無效 */
    if (out_rssi_dbm) *out_rssi_dbm = -127;
    return false;
}

static inline bool frame_is_imu28(const uint8_t *b)
{
    return (b[0] == 0x1C && b[1] == 0x03 && b[2] == 0x00);
}

static inline uint8_t frame_valid_len(const uint8_t *b)
{
    /* 你目前假設：總長度 = LEN + 2 */
    uint32_t v = (uint32_t)b[0] + 2U;
    if (v < 2U) v = 2U;
    if (v > PRINT_BYTES) v = PRINT_BYTES;
    return (uint8_t)v;
}

/* 一次組好整行，避免 per-byte printk */
#define LINE_BUF_SZ (110 + (PRINT_BYTES * 3))

static void print_packet_csv_rep(int64_t ts_ms, uint8_t freq_off, int8_t rssi_dbm,
                                 uint8_t rxmatch, uint8_t crcok, uint32_t rep,
                                 const uint8_t *buf, uint8_t len)
{
    char line[LINE_BUF_SZ];
    int pos = 0;

    pos += snprintk(line + pos, sizeof(line) - pos,
                    "PKT,%lld,%u,%u,%u,%d,%u,%u,",
                    ts_ms,
                    (uint32_t)(2400 + freq_off),
                    (uint32_t)freq_off,
                    (uint32_t)rxmatch,
                    (int)rssi_dbm,
                    (uint32_t)crcok,
                    (uint32_t)rep);

    for (uint8_t i = 0; i < len && pos < (int)sizeof(line) - 4; i++) {
        pos += snprintk(line + pos, sizeof(line) - pos,
                        (i == (len - 1)) ? "%02X" : "%02X ",
                        buf[i]);
    }

    pos += snprintk(line + pos, sizeof(line) - pos, "\n");
    printk("%s", line);
}

static void radio_init(void)
{
    NRF_RADIO->POWER = 0;
    k_busy_wait(500);
    NRF_RADIO->POWER = 1;

    NRF_RADIO->MODE = NRF_RADIO_MODE_BLE_2MBIT;

    NRF_RADIO->PCNF0 =
        (8UL << RADIO_PCNF0_LFLEN_Pos) |
        (0UL << RADIO_PCNF0_S0LEN_Pos) |
        (4UL << RADIO_PCNF0_S1LEN_Pos);

    NRF_RADIO->PCNF1 =
        (PRINT_BYTES << RADIO_PCNF1_MAXLEN_Pos) |
        (0UL        << RADIO_PCNF1_STATLEN_Pos) |
        (4UL        << RADIO_PCNF1_BALEN_Pos)   |
        (1UL        << RADIO_PCNF1_ENDIAN_Pos)  |
        (0UL        << RADIO_PCNF1_WHITEEN_Pos);

    NRF_RADIO->BASE1   = ADDR_BASE_1;
    NRF_RADIO->PREFIX0 = ADDR_PREFIX0;
    NRF_RADIO->TXADDRESS   = 1;
    NRF_RADIO->RXADDRESSES = (1UL << 1);

#if ENABLE_HW_CRC
    NRF_RADIO->CRCCNF  = 2;
    NRF_RADIO->CRCPOLY = 0x00011021;
    NRF_RADIO->CRCINIT = 0x0000FFFF;
#else
    NRF_RADIO->CRCCNF = 0;
#endif

    NRF_RADIO->SHORTS =
        RADIO_SHORTS_READY_START_Msk |
        RADIO_SHORTS_END_DISABLE_Msk;

    radio_disable_clean();
}

static bool radio_rx_once(uint8_t freq, uint32_t timeout_us,
                          int8_t *out_rssi_dbm, bool *out_rssi_valid,
                          uint8_t *out_rxmatch, uint8_t *out_crcok)
{
    NRF_RADIO->FREQUENCY = freq;
    NRF_RADIO->PACKETPTR = (uint32_t)rx_buffer;

    NRF_RADIO->EVENTS_END = 0;
    NRF_RADIO->EVENTS_DISABLED = 0;
    NRF_RADIO->EVENTS_READY = 0;
    NRF_RADIO->EVENTS_RSSIEND = 0;

    radio_disable_clean();
    NRF_RADIO->TASKS_RXEN = 1;

    /* 等 READY，避免 RSSI/收包狀態沒起來 */
    for (uint32_t i = 0; i < 300; i++) {
        if (NRF_RADIO->EVENTS_READY) break;
        k_busy_wait(1);
    }
    NRF_RADIO->EVENTS_READY = 0;

    int8_t rssi_dbm = -127;
    bool rssi_valid = radio_sample_rssi_dbm(&rssi_dbm);

    for (uint32_t i = 0; i < timeout_us; i++) {
        if (NRF_RADIO->EVENTS_END) {

            uint8_t rxmatch = (uint8_t)(NRF_RADIO->RXMATCH & 0xFF);

#if ENABLE_HW_CRC
            uint8_t crcok = (NRF_RADIO->CRCSTATUS ? 1 : 0);
#else
            uint8_t crcok = 255;
#endif
            if (out_rssi_dbm)   *out_rssi_dbm = rssi_dbm;
            if (out_rssi_valid) *out_rssi_valid = rssi_valid;
            if (out_rxmatch)    *out_rxmatch  = rxmatch;
            if (out_crcok)      *out_crcok    = crcok;

#if ENABLE_HW_CRC
            if (!NRF_RADIO->CRCSTATUS) {
                return false;
            }
#endif
            return true;
        }
        k_busy_wait(1);
    }

    radio_disable_clean();
    return false;
}

int main(void)
{
    if (device_is_ready(led.port)) {
        gpio_pin_configure_dt(&led, GPIO_OUTPUT_ACTIVE);
        gpio_pin_set_dt(&led, 0);
    }

    (void)usb_enable(NULL);

    for (int i = 0; i < 8; i++) {
        gpio_pin_toggle_dt(&led);
        k_sleep(K_MSEC(500));
    }
    gpio_pin_set_dt(&led, 0);

    printk("\n");
    printk("============================================\n");
    printk("PICO RF SNIFFER (pipe1, MAXLEN=%d)\n", PRINT_BYTES);
#if ENABLE_HW_CRC
    printk("CRC16: ON (poly=0x11021 init=0xFFFF)\n");
#else
    printk("CRC16: OFF\n");
#endif
    printk("Filter: CRCOK frames; RSSI>%d dBm only when RSSI is valid\n", RSSI_THRESHOLD_DBM);
    printk("Output: PKT,ts_ms,freq_mhz,off,pipe,rssi_dbm,crcok,rep,hex...\n");
#if FLUSH_SAME_AFTER_MS
    printk("Dedup: print on change; also flush same packet every %d ms with growing rep\n", FLUSH_SAME_AFTER_MS);
#else
    printk("Dedup: print first packet; then only print when content changes\n");
#endif
    printk("============================================\n");

    radio_init();

    size_t freq_idx = 0;

    while (1) {
        int64_t now_ms = k_uptime_get();

        uint8_t current_freq;
        if (lock_freq >= 0 && now_ms < lock_until_ms) {
            current_freq = (uint8_t)lock_freq;
        } else {
            lock_freq = -1;
            current_freq = target_freqs[freq_idx++];
            if (freq_idx >= (sizeof(target_freqs) / sizeof(target_freqs[0]))) {
                freq_idx = 0;
            }
        }

        int64_t dwell_end = k_uptime_get() + DWELL_MS;

        while (k_uptime_get() < dwell_end) {
            int8_t  rssi_dbm = -127;
            bool    rssi_valid = false;
            uint8_t rxmatch = 0xFF;
            uint8_t crcok   = 0xFF;

            bool ok = radio_rx_once(current_freq, RX_WAIT_US, &rssi_dbm, &rssi_valid, &rxmatch, &crcok);
            if (!ok) {
                continue;
            }

            /* 只有在 RSSI 有效時才做門檻過濾，避免 RSSI 失效造成全滅 */
            if (rssi_valid) {
                if (rssi_dbm <= RSSI_THRESHOLD_DBM) {
                    continue;
                }
            }

#if FILTER_IMU_ONLY
            if (!frame_is_imu28(rx_buffer)) {
                continue;
            }
#endif

            gpio_pin_toggle_dt(&led);

            uint8_t cur_len = frame_valid_len(rx_buffer);
            int64_t ts = k_uptime_get();

            bool same = false;
            if (have_last && cur_len == last_len && memcmp(rx_buffer, last_buf, cur_len) == 0) {
                same = true;
            }

            if (!have_last) {
                /* 第一包直接印，保證你看到東西 */
                memcpy(last_buf, rx_buffer, cur_len);
                last_len = cur_len;
                last_rep = 1;
                have_last = true;

                last_ts_ms = ts;
                last_freq_off = current_freq;
                last_rssi_dbm = rssi_dbm;
                last_rxmatch  = rxmatch;
                last_crcok    = crcok;
                last_flush_ms = ts;

                print_packet_csv_rep(last_ts_ms, last_freq_off, last_rssi_dbm,
                                     last_rxmatch, last_crcok, last_rep,
                                     last_buf, last_len);
            } else if (same) {
                last_rep++;

                /* 更新 meta，讓你看到「最新一次」的時間/頻點/RSSI */
                last_ts_ms = ts;
                last_freq_off = current_freq;
                last_rssi_dbm = rssi_dbm;
                last_rxmatch  = rxmatch;
                last_crcok    = crcok;

#if FLUSH_SAME_AFTER_MS
                if ((ts - last_flush_ms) >= FLUSH_SAME_AFTER_MS) {
                    last_flush_ms = ts;
                    print_packet_csv_rep(last_ts_ms, last_freq_off, last_rssi_dbm,
                                         last_rxmatch, last_crcok, last_rep,
                                         last_buf, last_len);
                }
#endif
                continue;
            } else {
                /* content changed: 印一次上一段 run 的最終狀態 */
                print_packet_csv_rep(last_ts_ms, last_freq_off, last_rssi_dbm,
                                     last_rxmatch, last_crcok, last_rep,
                                     last_buf, last_len);

                /* 開新 run，並立即印出新封包（不然你又會覺得沒輸出） */
                memcpy(last_buf, rx_buffer, cur_len);
                last_len = cur_len;
                last_rep = 1;

                last_ts_ms = ts;
                last_freq_off = current_freq;
                last_rssi_dbm = rssi_dbm;
                last_rxmatch  = rxmatch;
                last_crcok    = crcok;
                last_flush_ms = ts;

                print_packet_csv_rep(last_ts_ms, last_freq_off, last_rssi_dbm,
                                     last_rxmatch, last_crcok, last_rep,
                                     last_buf, last_len);
            }

            lock_freq = current_freq;
            lock_until_ms = k_uptime_get() + LOCK_ON_MS;
            dwell_end = k_uptime_get() + (DWELL_MS + 600);
        }
    }
}

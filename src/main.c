#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/usb/usb_device.h>
#include <zephyr/drivers/gpio.h>
#include <hal/nrf_radio.h>
#include <string.h>

#define ENABLE_HW_CRC        1   // 1=用硬體 CRC16 過濾並回報 CRCOK；0=不做 CRC（較不會漏，但噪聲多）
#define RSSI_THRESHOLD_DBM  -95  // 只記錄 RSSI 大於此值（例如 -80 > -95）
#define LOCK_ON_MS         1500  // 命中後鎖住該頻點多久
#define DWELL_MS            200  // 每個頻點掃描停留時間
#define RX_WAIT_US         5000  // 每次 RX window 等待 END 的時間
#define PRINT_BYTES          64  // 固定印 64 bytes（足夠容納 MAXLEN=55 的封包）

// 若你只想記錄 IMU(0x1C 0x03 0x00) 封包，把這個改成 1
#define FILTER_IMU_ONLY      0

// 你說配對時確認「只聽這三個頻點」
static const uint8_t target_freqs[] = { 1, 37, 77 };

/*
 * 你配對中的寄存器：
 * BASE0=0x43434343
 * PREFIX0=0x23C343C0  => bytes: C0 43 C3 23
 *
 * pipe0 prefix=C0 (BASE0)
 * pipe1 prefix=43 (BASE1)
 * pipe2 prefix=C3 (BASE1)
 * pipe3 prefix=23 (BASE1)
 */
#define ADDR_BASE_0       0x43434343UL
#define ADDR_BASE_1       0x43434343UL
#define ADDR_PREFIX0      0x23C343C0UL

// 我們要同時聽 pipe0~pipe3
#define RX_PIPES_MASK     ((1UL << 0) | (1UL << 1) | (1UL << 2) | (1UL << 3))

// LED
#define LED0_NODE DT_ALIAS(led0)
static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(LED0_NODE, gpios);

// RX buffer（DMA 要求 4-byte aligned）
static uint8_t rx_buffer[PRINT_BYTES] __aligned(4);

static int lock_freq = -1;
static int64_t lock_until_ms = 0;

/* ---------------- Dedup + run-length count ---------------- */
static bool have_last = false;
static uint8_t last_buf[PRINT_BYTES];
static uint32_t last_rep = 0;

/* 用「最後一次看到這包」的 meta 來印（比較貼近你 log 的 timestamp/freq） */
static int64_t last_ts_ms = 0;
static uint8_t last_freq_off = 0;
static int8_t last_rssi_dbm = -127;
static uint8_t last_rxmatch = 0xFF;
static uint8_t last_crcok = 0xFF;
/* ---------------------------------------------------------- */

static inline void radio_disable_clean(void)
{
    NRF_RADIO->EVENTS_DISABLED = 0;
    NRF_RADIO->TASKS_DISABLE = 1;
    while (NRF_RADIO->EVENTS_DISABLED == 0) { /* wait */ }
    NRF_RADIO->EVENTS_DISABLED = 0;
}

static inline int8_t radio_sample_rssi_dbm(void)
{
    NRF_RADIO->EVENTS_RSSIEND = 0;
    NRF_RADIO->TASKS_RSSISTART = 1;

    for (int i = 0; i < 200; i++) {
        if (NRF_RADIO->EVENTS_RSSIEND) break;
        k_busy_wait(1);
    }
    NRF_RADIO->EVENTS_RSSIEND = 0;

    return (int8_t)NRF_RADIO->RSSISAMPLE;
}

static inline bool frame_is_imu28(const uint8_t *b)
{
    return (b[0] == 0x1C && b[1] == 0x03 && b[2] == 0x00);
}

static void radio_init(void)
{
    NRF_RADIO->POWER = 0;
    k_busy_wait(500);
    NRF_RADIO->POWER = 1;

    // PHY: BLE 2Mbit (MODE=4)
    NRF_RADIO->MODE = NRF_RADIO_MODE_BLE_2MBIT;

    // PCNF0: 0x00040008 => LFLEN=8, S0LEN=0, S1LEN=4
    NRF_RADIO->PCNF0 =
        (8UL << RADIO_PCNF0_LFLEN_Pos) |
        (0UL << RADIO_PCNF0_S0LEN_Pos) |
        (4UL << RADIO_PCNF0_S1LEN_Pos);

    // PCNF1: MAXLEN=64, BALEN=4 => address length 5 bytes
    NRF_RADIO->PCNF1 =
        (PRINT_BYTES << RADIO_PCNF1_MAXLEN_Pos) |
        (0UL        << RADIO_PCNF1_STATLEN_Pos) |
        (4UL        << RADIO_PCNF1_BALEN_Pos)   |
        (1UL        << RADIO_PCNF1_ENDIAN_Pos)  |
        (0UL        << RADIO_PCNF1_WHITEEN_Pos);

    // Address: 開 pipe0~pipe3（依 PREFIX0 的 4 個 prefix）
    NRF_RADIO->BASE0   = ADDR_BASE_0;
    NRF_RADIO->BASE1   = ADDR_BASE_1;
    NRF_RADIO->PREFIX0 = ADDR_PREFIX0;

    NRF_RADIO->TXADDRESS   = 1;              // sniff 不太重要，保留
    NRF_RADIO->RXADDRESSES = RX_PIPES_MASK;  // ★關鍵：同時聽多個 pipe

#if ENABLE_HW_CRC
    NRF_RADIO->CRCCNF  = 2;
    NRF_RADIO->CRCPOLY = 0x00011021;
    NRF_RADIO->CRCINIT = 0x0000FFFF;
#else
    NRF_RADIO->CRCCNF = 0;
#endif

    // SHORTS: READY->START, END->DISABLE
    NRF_RADIO->SHORTS =
        RADIO_SHORTS_READY_START_Msk |
        RADIO_SHORTS_END_DISABLE_Msk;

    radio_disable_clean();
}

static bool radio_rx_once(uint8_t freq, uint32_t timeout_us,
                          int8_t *out_rssi_dbm, uint8_t *out_rxmatch, uint8_t *out_crcok)
{
    NRF_RADIO->FREQUENCY = freq;
    NRF_RADIO->PACKETPTR = (uint32_t)rx_buffer;

    NRF_RADIO->EVENTS_END = 0;
    NRF_RADIO->EVENTS_DISABLED = 0;
    NRF_RADIO->EVENTS_RSSIEND = 0;

    radio_disable_clean();
    NRF_RADIO->TASKS_RXEN = 1;

    int8_t rssi_dbm = radio_sample_rssi_dbm();

    for (uint32_t i = 0; i < timeout_us; i++) {
        if (NRF_RADIO->EVENTS_END) {
            if (rssi_dbm == -127) {
                rssi_dbm = radio_sample_rssi_dbm();
            }

            uint8_t rxmatch = (uint8_t)(NRF_RADIO->RXMATCH & 0xFF);

#if ENABLE_HW_CRC
            uint8_t crcok = (NRF_RADIO->CRCSTATUS ? 1 : 0);
#else
            uint8_t crcok = 255;
#endif

            if (out_rssi_dbm) *out_rssi_dbm = rssi_dbm;
            if (out_rxmatch)  *out_rxmatch  = rxmatch;
            if (out_crcok)    *out_crcok    = crcok;

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

static void print_packet_csv_rep(int64_t ts_ms, uint8_t freq_off, int8_t rssi_dbm,
                                 uint8_t rxmatch, uint8_t crcok, uint32_t rep,
                                 const uint8_t *buf)
{
    printk("PKT,%lld,%u,%u,%u,%d,%u,%u,",
           ts_ms,
           (uint32_t)(2400 + freq_off),
           (uint32_t)freq_off,
           (uint32_t)rxmatch,
           (int)rssi_dbm,
           (uint32_t)crcok,
           (uint32_t)rep);

    for (int i = 0; i < PRINT_BYTES; i++) {
        printk("%02X", buf[i]);
        if (i != PRINT_BYTES - 1) printk(" ");
    }
    printk("\n");
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
    printk("PICO RF SNIFFER (pipes mask=0x%02X, MAXLEN=%d)\n", (unsigned)RX_PIPES_MASK, PRINT_BYTES);
#if ENABLE_HW_CRC
    printk("CRC16: ON (poly=0x11021 init=0xFFFF)\n");
#else
    printk("CRC16: OFF\n");
#endif
#if FILTER_IMU_ONLY
    printk("Filter: IMU only (0x1C 0x03 0x00)\n");
#else
    printk("Filter: ALL CRCOK frames (RSSI>%d dBm)\n", RSSI_THRESHOLD_DBM);
#endif
    printk("Addr: BASE0=0x%08X BASE1=0x%08X PREFIX0=0x%08X\n",
           (unsigned)ADDR_BASE_0, (unsigned)ADDR_BASE_1, (unsigned)ADDR_PREFIX0);
    printk("Output: CSV  PKT,ts_ms,freq_mhz,off,pipe,rssi_dbm,crcok,rep,hex64\n");
    printk("Note: Only prints when packet content changes; rep counts consecutive identical packets.\n");
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
            int8_t rssi_dbm = -127;
            uint8_t rxmatch = 0xFF;
            uint8_t crcok   = 0xFF;

            bool ok = radio_rx_once(current_freq, RX_WAIT_US, &rssi_dbm, &rxmatch, &crcok);
            if (!ok) {
                continue;
            }

            if (rssi_dbm <= RSSI_THRESHOLD_DBM) {
                continue;
            }

#if FILTER_IMU_ONLY
            if (!frame_is_imu28(rx_buffer)) {
                continue;
            }
#endif

            gpio_pin_toggle_dt(&led);

            bool same = false;
            if (have_last && memcmp(rx_buffer, last_buf, PRINT_BYTES) == 0) {
                same = true;
            }

            int64_t ts = k_uptime_get();

            if (same) {
                last_rep++;
                last_ts_ms = ts;
                last_freq_off = current_freq;
                last_rssi_dbm = rssi_dbm;
                last_rxmatch  = rxmatch;
                last_crcok    = crcok;
            } else {
                if (have_last) {
                    print_packet_csv_rep(last_ts_ms, last_freq_off, last_rssi_dbm,
                                         last_rxmatch, last_crcok, last_rep,
                                         last_buf);
                }

                memcpy(last_buf, rx_buffer, PRINT_BYTES);
                have_last = true;
                last_rep = 1;

                last_ts_ms = ts;
                last_freq_off = current_freq;
                last_rssi_dbm = rssi_dbm;
                last_rxmatch  = rxmatch;
                last_crcok    = crcok;
            }

            lock_freq = current_freq;
            lock_until_ms = k_uptime_get() + LOCK_ON_MS;
            dwell_end = k_uptime_get() + (DWELL_MS + 600);
        }
    }
}

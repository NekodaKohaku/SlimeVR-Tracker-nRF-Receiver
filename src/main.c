#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/usb/usb_device.h>
#include <zephyr/drivers/gpio.h>
#include <hal/nrf_radio.h>
#include <string.h>

/* ========= 你可調的開關 ========= */
#define ENABLE_HW_CRC        1   // 開著算 CRC，但不要用 CRC 直接丟包（看 DROP_BAD_CRC）
#define DROP_BAD_CRC         0   // 0=CRC fail 也印出來(只標 crcok=0)；1=CRC fail 直接丟掉
#define RSSI_THRESHOLD_DBM  -95  // 先不使用 (本版不做 rssi 過濾)
#define LOCK_ON_MS         1500
#define DWELL_MS            200
#define RX_WAIT_US         8000  // 配對 burst 可能很短，稍微拉長
#define PRINT_BYTES          64
#define FILTER_IMU_ONLY      0
/* =============================== */

/*
 * 你說「配對狀態確認聽 1/37/77」，
 * 但為了避免 off-by-one / 你之前抓到過 4/78 的情況，先做 ±2 掃描。
 */
static const uint8_t target_freqs[] = {
    1-2, 1-1, 1, 1+1, 1+2,
    37-2, 37-1, 37, 37+1, 37+2,
    77-2, 77-1, 77, 77+1, 77+2,
    // 你之前抓到的典型點（保險）
    4, 78
};

/*
 * 依你 dump：
 * 40001510 ... 552c6a1e   (很像 BASE0)
 * 40001520 43434343 23c343c0 13e363a3 ...
 *
 * 先把 BASE0/BASE1/PREFIX0/PREFIX1 都填上，
 * 然後 pipes 0~7 全開做探索。
 */
#define ADDR_BASE_0       0x552C6A1EUL
#define ADDR_BASE_1       0x43434343UL
#define ADDR_PREFIX0      0x23C343C0UL  // AP0=C0 AP1=43 AP2=C3 AP3=23
#define ADDR_PREFIX1      0x13E363A3UL  // AP4=A3 AP5=63 AP6=E3 AP7=13

#define RX_PIPES_MASK     0xFFUL        // 先全開 pipe0~pipe7 探索

#define LED0_NODE DT_ALIAS(led0)
static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(LED0_NODE, gpios);

static uint8_t rx_buffer[PRINT_BYTES] __aligned(4);

static int lock_freq = -1;
static int64_t lock_until_ms = 0;

/* ---- Dedup + run-length ---- */
static bool have_last = false;
static uint8_t last_buf[PRINT_BYTES];
static uint32_t last_rep = 0;

static int64_t last_ts_ms = 0;
static uint8_t last_freq_off = 0;
static int8_t  last_rssi_dbm = -127;
static uint8_t last_rxmatch = 0xFF;
static uint8_t last_crcok = 0xFF;

static inline void radio_disable_clean(void)
{
    NRF_RADIO->EVENTS_DISABLED = 0;
    NRF_RADIO->TASKS_DISABLE = 1;
    while (NRF_RADIO->EVENTS_DISABLED == 0) { }
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

    // 依你 dump：MODE=4 (BLE 2M)
    NRF_RADIO->MODE = NRF_RADIO_MODE_BLE_2MBIT;

    // 依你 dump：PCNF0=0x00040008 => LFLEN=8, S0LEN=0, S1LEN=4
    NRF_RADIO->PCNF0 =
        (8UL << RADIO_PCNF0_LFLEN_Pos) |
        (0UL << RADIO_PCNF0_S0LEN_Pos) |
        (4UL << RADIO_PCNF0_S1LEN_Pos);

    // MAXLEN 用 64 探索（大於對方即可），BALEN=4 => 5-byte address
    NRF_RADIO->PCNF1 =
        (PRINT_BYTES << RADIO_PCNF1_MAXLEN_Pos) |
        (0UL        << RADIO_PCNF1_STATLEN_Pos) |
        (4UL        << RADIO_PCNF1_BALEN_Pos)   |
        (1UL        << RADIO_PCNF1_ENDIAN_Pos)  |
        (0UL        << RADIO_PCNF1_WHITEEN_Pos);

    NRF_RADIO->BASE0   = ADDR_BASE_0;
    NRF_RADIO->BASE1   = ADDR_BASE_1;
    NRF_RADIO->PREFIX0 = ADDR_PREFIX0;
    NRF_RADIO->PREFIX1 = ADDR_PREFIX1;

    NRF_RADIO->TXADDRESS   = 1;
    NRF_RADIO->RXADDRESSES = RX_PIPES_MASK;

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

#if ENABLE_HW_CRC && DROP_BAD_CRC
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
           (uint32_t)rxmatch,     // 這就是 pipe 編號
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
    printk("RF SNIFFER (pipes=0x%02X, MAXLEN=%d)\n", (unsigned)RX_PIPES_MASK, PRINT_BYTES);
    printk("MODE: BLE_2M  PCNF0(LFLEN=8,S1=4)  WHITE=OFF\n");
#if ENABLE_HW_CRC
    printk("CRC16: ON (poly=0x11021 init=0xFFFF)  DROP_BAD_CRC=%d\n", DROP_BAD_CRC);
#else
    printk("CRC16: OFF\n");
#endif
    printk("ADDR: BASE0=0x%08X BASE1=0x%08X PREFIX0=0x%08X PREFIX1=0x%08X\n",
           (unsigned)ADDR_BASE_0, (unsigned)ADDR_BASE_1,
           (unsigned)ADDR_PREFIX0, (unsigned)ADDR_PREFIX1);
    printk("Output: PKT,ts_ms,freq_mhz,off,pipe,rssi_dbm,crcok,rep,hex64\n");
    printk("Note: prints only when packet content changes; rep is run-length.\n");
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
                                         last_rxmatch, last_crcok, last_rep, last_buf);
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

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/usb/usb_device.h>
#include <zephyr/drivers/gpio.h>
#include <hal/nrf_radio.h>
#include <string.h>

/* ========================================================================
 * PICO 配對監聽器 (Pairing Sniffer) - 針對頭盔發出的廣播
 * ======================================================================== */

// 1. 物理層與校驗設定
#define ENABLE_HW_CRC        1   // 必須開啟！PICO 配對用的是標準 CRC-16
#define RSSI_THRESHOLD_DBM  -90  // 配對時距離通常很近，設高一點濾掉雜訊
#define LOCK_ON_MS          500  // 頻率跳很快，鎖定時間縮短一點以免跟丟
#define DWELL_MS            50   // 掃描速度加快 (原 200 -> 50) 以捕捉跳頻
#define RX_WAIT_US          2000 // 每次 RX window
#define PRINT_BYTES         32   // 配對包通常不長，32 bytes 夠了

// 2. 監聽地址 (Target: Headset Pairing Beacon)
// 根據分析：頭盔廣播地址為 0xE343434343
// Little Endian 在記憶體中為：43 43 43 43 E3
#define ADDR_BASE_0       0x43434343UL
#define ADDR_PREFIX0      0x000000E3UL   // Pipe 0 位於 Byte 0

// 3. 頻率表 (Pseudo-BLE Hopping)
// 根據寄存器與 BLE 規範偏移：2401 (Ch37-like), 2437 (Ch38-like), 2477 (Ch39-like)
static const uint8_t target_freqs[] = {
    1,   // 2401 MHz
    37,  // 2437 MHz
    77   // 2477 MHz
};

// LED 設定
#define LED0_NODE DT_ALIAS(led0)
static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(LED0_NODE, gpios);

// RX buffer
static uint8_t rx_buffer[PRINT_BYTES] __aligned(4);

// 掃描狀態變數
static int lock_freq = -1;
static int64_t lock_until_ms = 0;

/* ---------------- Dedup (去重複) 變數 ---------------- */
static bool have_last = false;
static uint8_t last_buf[PRINT_BYTES];
static uint32_t last_rep = 0;
static int64_t last_ts_ms = 0;
static uint8_t last_freq_off = 0;
static int8_t last_rssi_dbm = -127;
/* --------------------------------------------------- */

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

static void radio_init(void)
{
    NRF_RADIO->POWER = 0;
    k_busy_wait(500);
    NRF_RADIO->POWER = 1;

    // [關鍵修改] PHY: 改為 BLE 1Mbit (對應寄存器 MODE=0x04)
    NRF_RADIO->MODE = NRF_RADIO_MODE_BLE_1MBIT;

    // PCNF0: LFLEN=8, S0=0, S1=0 (標準 Raw 抓包配置)
    // 這樣我們可以抓到 Length Byte 和後面的 Payload
    NRF_RADIO->PCNF0 =
        (8UL << RADIO_PCNF0_LFLEN_Pos) |
        (0UL << RADIO_PCNF0_S0LEN_Pos) |
        (0UL << RADIO_PCNF0_S1LEN_Pos);

    // PCNF1: Little Endian, No Whitening (關鍵！PICO 配對沒開 Whitening)
    NRF_RADIO->PCNF1 =
        (PRINT_BYTES << RADIO_PCNF1_MAXLEN_Pos) |
        (0UL         << RADIO_PCNF1_STATLEN_Pos)|
        (4UL         << RADIO_PCNF1_BALEN_Pos)  | // Base Address = 4 Bytes
        (1UL         << RADIO_PCNF1_ENDIAN_Pos) | // Little Endian
        (0UL         << RADIO_PCNF1_WHITEEN_Pos); // Disable Whitening

    // [關鍵修改] Address: 設定為頭盔廣播地址 0xE343434343
    // 使用 Pipe 0
    NRF_RADIO->BASE0   = ADDR_BASE_0;
    NRF_RADIO->PREFIX0 = ADDR_PREFIX0;
    NRF_RADIO->TXADDRESS   = 0;       
    NRF_RADIO->RXADDRESSES = 1; // 只啟用 Pipe 0

    // [關鍵修改] CRC: PICO 即使在 BLE 模式下，依然使用 CRC-16 (0x11021)
#if ENABLE_HW_CRC
    NRF_RADIO->CRCCNF  = (RADIO_CRCCNF_LEN_Two << RADIO_CRCCNF_LEN_Pos); // 2 Bytes
    NRF_RADIO->CRCPOLY = 0x11021; // CCITT Poly
    NRF_RADIO->CRCINIT = 0xFFFF;  // Init Value
#else
    NRF_RADIO->CRCCNF = 0;
#endif

    // Shortcuts
    NRF_RADIO->SHORTS =
        RADIO_SHORTS_READY_START_Msk |
        RADIO_SHORTS_END_DISABLE_Msk;

    radio_disable_clean();
}

static bool radio_rx_once(uint8_t freq, uint32_t timeout_us,
                          int8_t *out_rssi_dbm, uint8_t *out_crcok)
{
    NRF_RADIO->FREQUENCY = freq;
    NRF_RADIO->PACKETPTR = (uint32_t)rx_buffer;

    NRF_RADIO->EVENTS_END = 0;
    NRF_RADIO->EVENTS_DISABLED = 0;
    
    radio_disable_clean();
    NRF_RADIO->TASKS_RXEN = 1;

    int8_t rssi_dbm = radio_sample_rssi_dbm();

    for (uint32_t i = 0; i < timeout_us; i++) {
        if (NRF_RADIO->EVENTS_END) {
            if (rssi_dbm == -127) rssi_dbm = radio_sample_rssi_dbm();

            uint8_t crcok = (NRF_RADIO->CRCSTATUS ? 1 : 0);
            if (out_rssi_dbm) *out_rssi_dbm = rssi_dbm;
            if (out_crcok)    *out_crcok    = crcok;

            // 只接受 CRC 正確的包 (因為我們是要抓有效 Payload)
            if (!NRF_RADIO->CRCSTATUS) return false;
            
            return true;
        }
        k_busy_wait(1);
    }
    
    radio_disable_clean();
    return false;
}

static void print_packet_csv_rep(int64_t ts_ms, uint8_t freq_off, int8_t rssi_dbm,
                                 uint8_t crcok, uint32_t rep, const uint8_t *buf)
{
    // CSV 格式: PKT, Timestamp, Freq, RSSI, CRC, Repetition, HexData...
    printk("PKT,%lld,%u,%d,%u,%u,",
           ts_ms, (uint32_t)freq_off, (int)rssi_dbm, (uint32_t)crcok, (uint32_t)rep);

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

    // 等待 USB 連線
    for (int i = 0; i < 5; i++) {
        gpio_pin_toggle_dt(&led);
        k_sleep(K_MSEC(500));
    }
    gpio_pin_set_dt(&led, 0);

    printk("\n");
    printk("============================================\n");
    printk("PICO PAIRING SNIFFER (Headset Beacon)\n");
    printk("Mode: Pseudo-BLE 1Mbit (No Whitening)\n");
    printk("Addr: 0xE343434343 (Listen for Headset)\n");
    printk("Freqs: 2401, 2437, 2477 MHz\n");
    printk("============================================\n");

    radio_init();

    size_t freq_idx = 0;

    while (1) {
        int64_t now_ms = k_uptime_get();
        uint8_t current_freq;

        // 鎖定頻率邏輯
        if (lock_freq >= 0 && now_ms < lock_until_ms) {
            current_freq = (uint8_t)lock_freq;
        } else {
            lock_freq = -1;
            current_freq = target_freqs[freq_idx++];
            if (freq_idx >= (sizeof(target_freqs) / sizeof(target_freqs[0]))) {
                freq_idx = 0;
            }
        }

        // 在此頻點停留 DWELL_MS
        int64_t dwell_end = k_uptime_get() + DWELL_MS;

        while (k_uptime_get() < dwell_end) {
            int8_t rssi_dbm = -127;
            uint8_t crcok   = 0;

            bool ok = radio_rx_once(current_freq, RX_WAIT_US, &rssi_dbm, &crcok);
            if (!ok) continue;

            if (rssi_dbm <= RSSI_THRESHOLD_DBM) continue;

            // 抓到了！LED 閃一下
            gpio_pin_toggle_dt(&led);

            /* --------- 去重複邏輯 --------- */
            bool same = false;
            if (have_last && memcmp(rx_buffer, last_buf, PRINT_BYTES) == 0) {
                same = true;
            }

            if (same) {
                last_rep++;
                last_ts_ms = k_uptime_get();
                last_freq_off = current_freq;
                last_rssi_dbm = rssi_dbm;
            } else {
                if (have_last) {
                    print_packet_csv_rep(last_ts_ms, last_freq_off, last_rssi_dbm,
                                         1, last_rep, last_buf);
                }
                memcpy(last_buf, rx_buffer, PRINT_BYTES);
                have_last = true;
                last_rep = 1;
                last_ts_ms = k_uptime_get();
                last_freq_off = current_freq;
                last_rssi_dbm = rssi_dbm;
            }

            // 命中後鎖定頻率 500ms，增加連續抓包機會
            lock_freq = current_freq;
            lock_until_ms = k_uptime_get() + LOCK_ON_MS;
            dwell_end = k_uptime_get() + DWELL_MS; 
        }
    }
}

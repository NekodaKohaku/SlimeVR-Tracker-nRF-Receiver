#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/usb/usb_device.h>
#include <zephyr/drivers/gpio.h>
#include <hal/nrf_radio.h>

#define ENABLE_HW_CRC        1   // 1=用硬體 CRC16 過濾並回報 CRCOK；0=不做 CRC（較不會漏，但噪聲多）
#define RSSI_THRESHOLD_DBM  -95  // 只記錄 RSSI 大於此值（例如 -80 > -95）
#define LOCK_ON_MS         1500  // 命中後鎖住該頻點多久
#define DWELL_MS            200  // 每個頻點掃描停留時間
#define RX_WAIT_US         5000  // 每次 RX window 等待 END 的時間
#define PRINT_BYTES          64  // 固定印 64 bytes（足夠容納 MAXLEN=55 的封包）

// 若你只想記錄 IMU(0x1C 0x03 0x00) 封包，把這個改成 1
#define FILTER_IMU_ONLY      0

// 你實測出現的低頻點 + 高頻 hopping 集合（依你貼的頻點補齊）
static const uint8_t target_freqs[] = {
    // 低頻/同步候選（你量到 2402/2404/2408）
    2, 8,

    // 高頻集合（你量到 2434~2480，偏偶數）
    34, 36, 40, 42, 44, 46, 48, 50, 52, 54, 56, 58,
    60, 62, 64, 66, 68, 70, 72, 74, 76, 80,
};

// 依你 SWD dump：pipe1 使用 BASE1 + PREFIX0.AP1(=0x00)
#define ADDR_BASE_1       0xD235CF35UL
#define ADDR_PREFIX0      0x23C300C0UL   // pipe0=C0, pipe1=00, pipe2=C3, pipe3=23

// LED
#define LED0_NODE DT_ALIAS(led0)
static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(LED0_NODE, gpios);

// RX buffer（DMA 要求 4-byte aligned）
static uint8_t rx_buffer[PRINT_BYTES] __aligned(4);

static int lock_freq = -1;
static int64_t lock_until_ms = 0;

static inline void radio_disable_clean(void)
{
    NRF_RADIO->EVENTS_DISABLED = 0;
    NRF_RADIO->TASKS_DISABLE = 1;
    while (NRF_RADIO->EVENTS_DISABLED == 0) { /* wait */ }
    NRF_RADIO->EVENTS_DISABLED = 0;
}

static inline int8_t radio_sample_rssi_dbm(void)
{
    // 要求：RADIO 在 RX 狀態（TASKS_RXEN 已下）
    NRF_RADIO->EVENTS_RSSIEND = 0;
    NRF_RADIO->TASKS_RSSISTART = 1;

    // RSSI 取樣通常很快（幾十 us），但我們加個小 timeout 保險
    for (int i = 0; i < 200; i++) {
        if (NRF_RADIO->EVENTS_RSSIEND) break;
        k_busy_wait(1);
    }
    NRF_RADIO->EVENTS_RSSIEND = 0;

    // 直接讀負值 dBm（不要取負號）
    return (int8_t)NRF_RADIO->RSSISAMPLE;
}

static inline bool frame_is_imu28(const uint8_t *b)
{
    // 你的 v2.0：len=0x1C, header=0x03 0x00
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

    // PCNF1: 固定 MAXLEN=64（可涵蓋對方偶爾用 0x23 / 0x37）
    // STATLEN=0, BALEN=4 (=> address length 5 bytes), ENDIAN=1, WHITEEN=0
    NRF_RADIO->PCNF1 =
        (PRINT_BYTES << RADIO_PCNF1_MAXLEN_Pos) |
        (0UL        << RADIO_PCNF1_STATLEN_Pos) |
        (4UL        << RADIO_PCNF1_BALEN_Pos)   |
        (1UL        << RADIO_PCNF1_ENDIAN_Pos)  |
        (0UL        << RADIO_PCNF1_WHITEEN_Pos);

    // Address: pipe1 only
    NRF_RADIO->BASE1   = ADDR_BASE_1;
    NRF_RADIO->PREFIX0 = ADDR_PREFIX0;
    NRF_RADIO->TXADDRESS   = 1;          // 不是 sniff 必需，但照你 dump 設定
    NRF_RADIO->RXADDRESSES = (1UL << 1); // 只開 pipe1

#if ENABLE_HW_CRC
    // CRC16: CRCCNF=2, POLY=0x11021, INIT=0xFFFF
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

    // 保持乾淨狀態
    radio_disable_clean();
}

static bool radio_rx_once(uint8_t freq, uint32_t timeout_us,
                          int8_t *out_rssi_dbm, uint8_t *out_rxmatch, uint8_t *out_crcok)
{
    NRF_RADIO->FREQUENCY = freq;
    NRF_RADIO->PACKETPTR = (uint32_t)rx_buffer;

    // 清事件
    NRF_RADIO->EVENTS_END = 0;
    NRF_RADIO->EVENTS_DISABLED = 0;
    NRF_RADIO->EVENTS_RSSIEND = 0;

    // 先 disable 確保狀態機乾淨
    radio_disable_clean();

    // 啟動 RX
    NRF_RADIO->TASKS_RXEN = 1;

    // 先觸發一次 RSSI 取樣（不一定等 READY）
    int8_t rssi_dbm = radio_sample_rssi_dbm();

    // 等 END 或 timeout
    for (uint32_t i = 0; i < timeout_us; i++) {
        if (NRF_RADIO->EVENTS_END) {
            // 確保 RSSI 有更新一次（若剛剛那次沒成功，再補一次）
            if (rssi_dbm == -127) {
                rssi_dbm = radio_sample_rssi_dbm();
            }

            uint8_t rxmatch = (uint8_t)(NRF_RADIO->RXMATCH & 0xFF);

#if ENABLE_HW_CRC
            uint8_t crcok = (NRF_RADIO->CRCSTATUS ? 1 : 0);
#else
            uint8_t crcok = 255; // unknown
#endif

            if (out_rssi_dbm) *out_rssi_dbm = rssi_dbm;
            if (out_rxmatch)  *out_rxmatch  = rxmatch;
            if (out_crcok)    *out_crcok    = crcok;

#if ENABLE_HW_CRC
            if (!NRF_RADIO->CRCSTATUS) {
                return false; // 只要 CRC 失敗，直接視為無效（避免噪聲）
            }
#endif
            return true;
        }
        k_busy_wait(1);
    }

    // timeout
    radio_disable_clean();
    return false;
}

static void print_packet_csv(int64_t ts_ms, uint8_t freq_off, int8_t rssi_dbm,
                             uint8_t rxmatch, uint8_t crcok, const uint8_t *buf)
{
    // CSV：PKT,ts_ms,freq_mhz,off,pipe,rssi_dbm,crcok,hex64...
    printk("PKT,%lld,%u,%u,%u,%d,%u,",
           ts_ms,
           (uint32_t)(2400 + freq_off),
           (uint32_t)freq_off,
           (uint32_t)rxmatch,
           (int)rssi_dbm,
           (uint32_t)crcok);

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

    // 延遲啟動（方便你開 terminal/開始紀錄）
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
#if FILTER_IMU_ONLY
    printk("Filter: IMU only (0x1C 0x03 0x00)\n");
#else
    printk("Filter: ALL CRCOK frames (RSSI>%d dBm)\n", RSSI_THRESHOLD_DBM);
#endif
    printk("Output: CSV  PKT,ts_ms,freq_mhz,off,pipe,rssi_dbm,crcok,hex64\n");
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

            // RSSI 門檻（-60 > -95 成立）
            if (rssi_dbm <= RSSI_THRESHOLD_DBM) {
                continue;
            }

#if FILTER_IMU_ONLY
            if (!frame_is_imu28(rx_buffer)) {
                continue;
            }
#endif

            // 命中：輸出 + LED + lock-on
            gpio_pin_toggle_dt(&led);

            int64_t ts = k_uptime_get();
            print_packet_csv(ts, current_freq, rssi_dbm, rxmatch, crcok, rx_buffer);

            // 命中後鎖住此頻點一段時間（有助於連續抓包/跟頻）
            lock_freq = current_freq;
            lock_until_ms = k_uptime_get() + LOCK_ON_MS;

            // 命中後 dwell 也稍微延長，增加連續抓到的機率
            dwell_end = k_uptime_get() + (DWELL_MS + 600);
        }
    }
}

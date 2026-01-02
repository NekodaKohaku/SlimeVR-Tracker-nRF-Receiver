#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/usb/usb_device.h>
#include <zephyr/drivers/gpio.h>
#include <hal/nrf_radio.h>
#include <hal/nrf_gpio.h>

#define ENABLE_HW_CRC   1   // 1=打開 CRC16 硬體過濾；0=關閉（不建議，假陽性會變多）

// 你目前的掃描頻點（offset MHz；實際頻率 = 2400 + offset）
static const uint8_t target_freqs[] = {
    2, 4, 8,          // 2402/2404/2408：你實測真的出現
    34, 36, 40, 42, 44, 46, 48, 50, 52, 54, 56, 58, 60, 62, 64, 66, 68, 70, 72, 74, 76, 78, 80
};

// 你目前鎖定的地址參數（Pipe 1）
#define ADDR_BASE_1       0xD235CF35UL
#define ADDR_PREFIX_0     0x23C300C0UL   // 你原本的 PREFIX0（包含多個 pipe 的 prefix bytes）
// 只開 pipe1 時，只需要確保 pipe1 的 prefix byte 正確存在於 PREFIX0 對應位置即可

// LED
#define LED0_NODE DT_ALIAS(led0)
static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(LED0_NODE, gpios);

// RX buffer：4-byte aligned
static uint8_t rx_buffer[64] __aligned(4);

// lock-on（命中後暫時鎖住該頻點，提高連續收包）
static int  lock_freq = -1;
static int64_t lock_until_ms = 0;

static inline void radio_disable_clean(void)
{
    NRF_RADIO->EVENTS_DISABLED = 0;
    NRF_RADIO->TASKS_DISABLE = 1;
    while (NRF_RADIO->EVENTS_DISABLED == 0) {
        /* wait */
    }
    NRF_RADIO->EVENTS_DISABLED = 0;
}

static void radio_init(void)
{
    NRF_RADIO->POWER = 0;
    k_busy_wait(500);
    NRF_RADIO->POWER = 1;

    // PHY
    NRF_RADIO->MODE = NRF_RADIO_MODE_BLE_2MBIT;

    // PCNF0: LFLEN=8, S0LEN=0, S1LEN=4
    NRF_RADIO->PCNF0 =
        (8UL << RADIO_PCNF0_LFLEN_Pos) |
        (0UL << RADIO_PCNF0_S0LEN_Pos) |
        (4UL << RADIO_PCNF0_S1LEN_Pos);

    // PCNF1：修正 STATLEN=0；MAXLEN=0x23 (35)；BALEN=4 (=> 5 bytes address)
    NRF_RADIO->PCNF1 =
        (0x23UL << RADIO_PCNF1_MAXLEN_Pos) |
        (0x00UL << RADIO_PCNF1_STATLEN_Pos) |
        (4UL    << RADIO_PCNF1_BALEN_Pos)  |
        (1UL    << RADIO_PCNF1_ENDIAN_Pos) |
        (0UL    << RADIO_PCNF1_WHITEEN_Pos);

    // 地址：Pipe1 使用 BASE1
    NRF_RADIO->BASE1   = ADDR_BASE_1;
    NRF_RADIO->PREFIX0 = ADDR_PREFIX_0;

    // 只開 pipe1，降低假陽性
    NRF_RADIO->RXADDRESSES = (1UL << 1);

#if ENABLE_HW_CRC
    // CRC16 (CCITT family)；若你確定空中 CRC 關閉，改 ENABLE_HW_CRC=0
    NRF_RADIO->CRCCNF  = 2;            // 2 bytes
    NRF_RADIO->CRCPOLY = 0x00011021;   // 注意：照寄存器寫法
    NRF_RADIO->CRCINIT = 0x0000FFFF;
#else
    NRF_RADIO->CRCCNF = 0;
#endif

    // SHORTS：READY->START, END->DISABLE
    NRF_RADIO->SHORTS =
        RADIO_SHORTS_READY_START_Msk |
        RADIO_SHORTS_END_DISABLE_Msk;

    // 清狀態
    radio_disable_clean();
}

static bool radio_rx_once(uint8_t freq, uint32_t timeout_us, int8_t *out_rssi_dbm, uint8_t *out_rxmatch)
{
    NRF_RADIO->FREQUENCY = freq;

    // 設定 buffer
    NRF_RADIO->PACKETPTR = (uint32_t)rx_buffer;

    // 清事件
    NRF_RADIO->EVENTS_END = 0;
    NRF_RADIO->EVENTS_READY = 0;
    NRF_RADIO->EVENTS_RSSIEND = 0;
    NRF_RADIO->EVENTS_DISABLED = 0;

    // 先 disable 確保乾淨
    NRF_RADIO->TASKS_DISABLE = 1;
    while (NRF_RADIO->EVENTS_DISABLED == 0) { /* wait */ }
    NRF_RADIO->EVENTS_DISABLED = 0;

    // 啟動 RX
    NRF_RADIO->TASKS_RXEN = 1;

    // 開始 RSSI 取樣（不一定要等 READY，這樣更省事）
    NRF_RADIO->TASKS_RSSISTART = 1;

    // 等 END（或 timeout）
    for (uint32_t i = 0; i < timeout_us; i++) {
        if (NRF_RADIO->EVENTS_END) {
            // RSSI 可能還沒 RSIEND，盡量讀到就好
            int8_t rssi_dbm = (int8_t)NRF_RADIO->RSSISAMPLE;
            uint8_t rxmatch = (uint8_t)(NRF_RADIO->RXMATCH & 0xFF);

            if (out_rssi_dbm) *out_rssi_dbm = rssi_dbm;
            if (out_rxmatch)  *out_rxmatch  = rxmatch;

#if ENABLE_HW_CRC
            if (NRF_RADIO->CRCSTATUS == 0) {
                return false; // CRC 不過就丟掉
            }
#endif
            return true;
        }
        k_busy_wait(1);
    }

    // timeout：停掉 radio
    radio_disable_clean();
    return false;
}

static inline bool frame_looks_like_imu28(const uint8_t *b)
{
    // 你的 v2.0：len=0x1C, header=0x03 0x00
    return (b[0] == 0x1C && b[1] == 0x03 && b[2] == 0x00);
}

int main(void)
{
    if (device_is_ready(led.port)) {
        gpio_pin_configure_dt(&led, GPIO_OUTPUT_ACTIVE);
        gpio_pin_set_dt(&led, 0);
    }

    (void)usb_enable(NULL);

    // 延遲啟動
    for (int i = 0; i < 8; i++) {
        gpio_pin_toggle_dt(&led);
        k_sleep(K_MSEC(500));
    }
    gpio_pin_set_dt(&led, 0);

    printk("\n\n");
    printk("============================================\n");
    printk("PICO RF SNIFFER (Zephyr/nRF52840)\n");
#if ENABLE_HW_CRC
    printk("HW CRC: ON (CRC16 0x11021 init=0xFFFF)\n");
#else
    printk("HW CRC: OFF (software filter only)\n");
#endif
    printk("Pipe: 1 only\n");
    printk("============================================\n");

    radio_init();

    size_t freq_idx = 0;

    while (1) {
        int64_t now_ms = k_uptime_get();

        // lock-on：命中後鎖住頻點一段時間
        uint8_t current_freq;
        if (lock_freq >= 0 && now_ms < lock_until_ms) {
            current_freq = (uint8_t)lock_freq;
        } else {
            lock_freq = -1;
            current_freq = target_freqs[freq_idx];
            freq_idx++;
            if (freq_idx >= (sizeof(target_freqs) / sizeof(target_freqs[0]))) {
                freq_idx = 0;
            }
        }

        // 每個頻點 dwell（若 lock-on，會一直回到同頻點）
        int64_t dwell_end_ms = now_ms + 200; // 0.2s
        while (k_uptime_get() < dwell_end_ms) {
            int8_t rssi_dbm = -127;
            uint8_t rxmatch = 0xFF;

            bool ok = radio_rx_once(current_freq, 5000, &rssi_dbm, &rxmatch);

            if (!ok) {
                continue;
            }

            // 軟體濾波：避免假陽性（即使 CRC 開了也保留這層）
            if (!frame_looks_like_imu28(rx_buffer)) {
                continue;
            }

            // RSSI 門檻（注意：rssi_dbm 是負值）
            if (rssi_dbm <= -90) {
                continue;
            }

            // 命中：印出資訊
            gpio_pin_toggle_dt(&led);

            int64_t ts = k_uptime_get();
            printk("\n[JACKPOT] t=%lldms  F=%dMHz(off=%u)  RSSI=%ddBm  RXMATCH=%u\n",
                   ts, 2400 + current_freq, current_freq, rssi_dbm, rxmatch);

            printk("Data(32): ");
            for (int k = 0; k < 32; k++) {
                printk("%02X ", rx_buffer[k]);
            }
            printk("\n");

            // lock-on：命中後鎖 1500ms
            lock_freq = current_freq;
            lock_until_ms = k_uptime_get() + 1500;

            // dwell 也延長一點（讓它在同頻點上多吃幾包）
            dwell_end_ms = k_uptime_get() + 800;
        }
    }
}

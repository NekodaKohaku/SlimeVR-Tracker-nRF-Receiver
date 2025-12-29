/*
 * Pico Tracker HUNTER v12 (User Config Special)
 * Config: 使用你提供的 CONFIG_LOG_MODE_IMMEDIATE=y
 * Logic:  開機硬延遲 4 秒 (等待 USB 連線) -> 開始掃描
 * Params: Freq/CRC/S1/Endian 全數修正 (Based on pyOCD)
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <hal/nrf_radio.h>
#include <zephyr/usb/usb_device.h>
#include <zephyr/drivers/gpio.h>

// 1. 頻率表 (Correct: 2446, 2454, 2472, 2480)
static const uint8_t target_freqs[] = {46, 54, 72, 80}; 

// 2. 地址 (Correct)
#define ADDR_BASE      0x552C6A1EUL
#define ADDR_PREFIX    0xC0

// 3. LED 定義
#define LED0_NODE DT_ALIAS(led0)
static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(LED0_NODE, gpios);

static uint8_t rx_buffer[64];

void radio_init(void)
{
    NRF_RADIO->POWER = 0;
    k_busy_wait(500);
    NRF_RADIO->POWER = 1;

    NRF_RADIO->MODE = NRF_RADIO_MODE_BLE_2MBIT;

    // PCNF0: S1=4 (Verified)
    NRF_RADIO->PCNF0 = (8UL << RADIO_PCNF0_LFLEN_Pos) | 
                       (0UL << RADIO_PCNF0_S0LEN_Pos) | 
                       (4UL << RADIO_PCNF0_S1LEN_Pos);

    // PCNF1: Big Endian (Verified)
    NRF_RADIO->PCNF1 = (55UL << RADIO_PCNF1_MAXLEN_Pos) |
                       (55UL << RADIO_PCNF1_STATLEN_Pos) |
                       (4UL  << RADIO_PCNF1_BALEN_Pos) |
                       (1UL  << RADIO_PCNF1_ENDIAN_Pos) | 
                       (0UL  << RADIO_PCNF1_WHITEEN_Pos);

    NRF_RADIO->BASE0 = ADDR_BASE;
    NRF_RADIO->PREFIX0 = (ADDR_PREFIX << 0);
    NRF_RADIO->TXADDRESS = 0;
    NRF_RADIO->RXADDRESSES = 1; 

    // CRC: 0x1021 (Verified)
    NRF_RADIO->CRCCNF = (RADIO_CRCCNF_LEN_Two << RADIO_CRCCNF_LEN_Pos) | 
                        (0UL << RADIO_CRCCNF_SKIPADDR_Pos);
    NRF_RADIO->CRCINIT = 0xFFFF;
    NRF_RADIO->CRCPOLY = 0x1021; 

    NRF_RADIO->SHORTS = RADIO_SHORTS_READY_START_Msk | RADIO_SHORTS_END_DISABLE_Msk;
}

int main(void)
{
    // LED 初始化
    if (device_is_ready(led.port)) {
        gpio_pin_configure_dt(&led, GPIO_OUTPUT_ACTIVE);
        gpio_pin_set_dt(&led, 0); // 0 亮 (或 1 亮，視板子而定，先假設 Low Active)
    }

    // 啟動 USB (你的 config 會讓它自動 init，這裡再次確保 enable)
    usb_enable(NULL);
    
    // ★ 關鍵延遲：配合你的 IMMEDIATE 模式 ★
    // 因為 IMMEDIATE 模式不緩衝，所以我們必須等電腦連上才能印字。
    // 這裡讓 LED 閃爍 4 秒 (8次)，請在這段時間打開串口軟體。
    for(int i=0; i<8; i++) {
        gpio_pin_toggle_dt(&led);
        k_sleep(K_MSEC(500));
    }
    gpio_pin_set_dt(&led, 0); // 確保燈是亮的狀態進入 Loop

    printk("\n\n");
    printk("============================================\n");
    printk(">>> HUNTER v12 (User Config)             <<<\n");
    printk(">>> Configured for IMMEDIATE LOG MODE    <<<\n");
    printk("============================================\n");

    radio_init();

    int freq_idx = 0;

    while (1) {
        int current_freq = target_freqs[freq_idx];
        NRF_RADIO->FREQUENCY = current_freq;
        
        // 顯示掃描中...證明程式活著
        printk(">>> Scanning %d MHz...\n", 2400 + current_freq);

        int64_t end_time = k_uptime_get() + 2000;

        while (k_uptime_get() < end_time) {
            
            NRF_RADIO->PACKETPTR = (uint32_t)rx_buffer;
            NRF_RADIO->EVENTS_END = 0;
            NRF_RADIO->TASKS_RXEN = 1;

            bool received = false;
            for (int i = 0; i < 20000; i++) { // 20ms timeout
                if (NRF_RADIO->EVENTS_END) {
                    received = true;
                    break;
                }
                k_busy_wait(1);
            }

            if (received) {
                if (NRF_RADIO->CRCSTATUS == 1) {
                    gpio_pin_toggle_dt(&led); // 抓到訊號閃一下

                    int8_t rssi = -(int8_t)NRF_RADIO->RSSISAMPLE; 
                    printk("\n!!! [JACKPOT] Freq %d | RSSI %d !!!\n", 2400 + current_freq, rssi);
                    printk("Data: ");
                    for(int k=0; k<32; k++) printk("%02X ", rx_buffer[k]);
                    printk("\n");
                    
                    end_time += 200; 
                }
                NRF_RADIO->EVENTS_END = 0;
            } else {
                NRF_RADIO->TASKS_DISABLE = 1;
                while (NRF_RADIO->EVENTS_DISABLED == 0);
            }
        }

        freq_idx++;
        if (freq_idx >= sizeof(target_freqs) / sizeof(target_freqs[0])) {
            freq_idx = 0;
        }
    }
}

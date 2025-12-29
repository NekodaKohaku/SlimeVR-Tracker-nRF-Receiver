/*
 * Pico Tracker HUNTER v13 (Dual Address Scan)
 * 策略：同時監聽 "正向" 與 "反向" 地址，解決 Endian 不確定性
 * Pipe 0: 0x552C6A1E (原本的)
 * Pipe 1: 0x1E6A2C55 (反轉的)
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <hal/nrf_radio.h>
#include <zephyr/usb/usb_device.h>
#include <zephyr/drivers/gpio.h>

// 1. 頻率表
static const uint8_t target_freqs[] = {46, 54, 72, 80}; 

// 2. 地址定義 (兩種可能性)
#define ADDR_BASE_NORMAL  0x552C6A1EUL
#define ADDR_BASE_REV     0x1E6A2C55UL // Byte Swapped
#define ADDR_PREFIX       0xC0

// 3. LED
#define LED0_NODE DT_ALIAS(led0)
static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(LED0_NODE, gpios);

static uint8_t rx_buffer[64];

void radio_init(void)
{
    NRF_RADIO->POWER = 0;
    k_busy_wait(500);
    NRF_RADIO->POWER = 1;

    NRF_RADIO->MODE = NRF_RADIO_MODE_BLE_2MBIT;

    // PCNF0: S1=4
    NRF_RADIO->PCNF0 = (8UL << RADIO_PCNF0_LFLEN_Pos) | 
                       (0UL << RADIO_PCNF0_S0LEN_Pos) | 
                       (4UL << RADIO_PCNF0_S1LEN_Pos);

    // PCNF1: Big Endian
    NRF_RADIO->PCNF1 = (55UL << RADIO_PCNF1_MAXLEN_Pos) |
                       (55UL << RADIO_PCNF1_STATLEN_Pos) |
                       (4UL  << RADIO_PCNF1_BALEN_Pos) |
                       (1UL  << RADIO_PCNF1_ENDIAN_Pos) | 
                       (0UL  << RADIO_PCNF1_WHITEEN_Pos);

    // === 設定雙地址 ===
    // Pipe 0: 正向 (我們原本猜的)
    NRF_RADIO->BASE0 = ADDR_BASE_NORMAL;
    NRF_RADIO->PREFIX0 = (ADDR_PREFIX << 0) | (ADDR_PREFIX << 8); // Prefix Byte 0 & 1 都設 C0

    // Pipe 1: 反向 (備案)
    NRF_RADIO->BASE1 = ADDR_BASE_REV;
    
    // 啟用 Pipe 0 和 Pipe 1
    NRF_RADIO->RXADDRESSES = 3; // Binary 0011 -> Enable Pipe 0 & 1

    // CRC
    NRF_RADIO->CRCCNF = (RADIO_CRCCNF_LEN_Two << RADIO_CRCCNF_LEN_Pos) | 
                        (0UL << RADIO_CRCCNF_SKIPADDR_Pos);
    NRF_RADIO->CRCINIT = 0xFFFF;
    NRF_RADIO->CRCPOLY = 0x1021; 

    NRF_RADIO->SHORTS = RADIO_SHORTS_READY_START_Msk | RADIO_SHORTS_END_DISABLE_Msk;
}

int main(void)
{
    if (device_is_ready(led.port)) {
        gpio_pin_configure_dt(&led, GPIO_OUTPUT_ACTIVE);
        gpio_pin_set_dt(&led, 0); 
    }

    usb_enable(NULL);
    
    // 延遲啟動，確保你看得到 Log
    for(int i=0; i<8; i++) {
        gpio_pin_toggle_dt(&led);
        k_sleep(K_MSEC(500));
    }
    gpio_pin_set_dt(&led, 0); 

    printk("\n\n");
    printk("============================================\n");
    printk(">>> HUNTER v13 (Dual Address Mode)       <<<\n");
    printk(">>> Listening on Pipe 0 (Normal)         <<<\n");
    printk(">>> Listening on Pipe 1 (Reversed)       <<<\n");
    printk("============================================\n");

    radio_init();

    int freq_idx = 0;

    while (1) {
        int current_freq = target_freqs[freq_idx];
        NRF_RADIO->FREQUENCY = current_freq;
        
        printk(">>> Scanning %d MHz...\n", 2400 + current_freq);

        int64_t end_time = k_uptime_get() + 2000;

        while (k_uptime_get() < end_time) {
            
            NRF_RADIO->PACKETPTR = (uint32_t)rx_buffer;
            NRF_RADIO->EVENTS_END = 0;
            NRF_RADIO->TASKS_RXEN = 1;

            bool received = false;
            for (int i = 0; i < 20000; i++) { 
                if (NRF_RADIO->EVENTS_END) {
                    received = true;
                    break;
                }
                k_busy_wait(1);
            }

            if (received) {
                if (NRF_RADIO->CRCSTATUS == 1) {
                    gpio_pin_toggle_dt(&led);

                    // 檢查是哪個 Pipe 抓到的
                    int pipe = 0;
                    if (NRF_RADIO->RXMATCH == 1) pipe = 1;

                    int8_t rssi = -(int8_t)NRF_RADIO->RSSISAMPLE; 
                    
                    printk("\n!!! [JACKPOT] Pipe %d | Freq %d | RSSI %d !!!\n", pipe, 2400 + current_freq, rssi);
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

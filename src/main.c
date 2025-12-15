/*
 * Copyright (c) 2025 White Cat DIY
 * Pico Tracker Waker - "The Alarm Clock"
 * Target Address: 0xC0552C6A1E
 * Payload: 0x00 0x01 (Wake Up / Keep-Alive)
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <hal/nrf_radio.h>
#include <zephyr/usb/usb_device.h>
#include <zephyr/drivers/uart.h>

#define CONSOLE_DEVICE_LABEL DT_CHOSEN(zephyr_console)

// 🔑 目標地址 (逆向搜出的)
#define TARGET_BASE_ADDR  0x552c6a1eUL
#define TARGET_PREFIX     0xC0

// 📡 攻擊頻率表 (根據之前的觀測)
// 我們輪流在這幾個頻率發射，確保 Tracker 跳到哪都能聽到
static const int target_channels[] = {1, 37, 77, 40}; 
#define CH_COUNT 4

// 📦 封包緩衝區
static uint8_t tx_packet[32];

void radio_tx_setup(int channel)
{
    // 1. 先停用 Radio
    NRF_RADIO->TASKS_DISABLE = 1;
    while (NRF_RADIO->EVENTS_DISABLED == 0);
    NRF_RADIO->EVENTS_DISABLED = 0;

    // 2. 設定頻率
    NRF_RADIO->FREQUENCY = channel;
    
    // 3. 設定 2Mbit ESB 模式 (私有協議)
    NRF_RADIO->MODE = RADIO_MODE_MODE_Nrf_2Mbit; 

    // 4. 設定地址 (The Golden Key)
    NRF_RADIO->BASE0 = TARGET_BASE_ADDR;
    NRF_RADIO->PREFIX0 = TARGET_PREFIX;
    NRF_RADIO->TXADDRESS = 0; // 使用 Logical Address 0 發射
    NRF_RADIO->RXADDRESSES = 1;

    // 5. 設定封包格式 (標準 ESB, 無 Dynamic Payload Length)
    NRF_RADIO->PCNF0 = 0;
    
    // MaxLen=32, Balen=4, Little Endian, No Whitening
    NRF_RADIO->PCNF1 = (32 << RADIO_PCNF1_MAXLEN_Pos) | 
                       (4 << RADIO_PCNF1_BALEN_Pos) | 
                       (RADIO_PCNF1_ENDIAN_Little << RADIO_PCNF1_ENDIAN_Pos) |
                       (0 << RADIO_PCNF1_WHITEEN_Pos); 

    // 6. 設定 CRC (非常重要！CRC 錯了 Tracker 會直接拒收)
    // 根據逆向結果：CRC-16-CCITT
    NRF_RADIO->CRCCNF = (RADIO_CRCCNF_LEN_Two << RADIO_CRCCNF_LEN_Pos); 
    NRF_RADIO->CRCINIT = 0xFFFF;      
    NRF_RADIO->CRCPOLY = 0x11021;      
    
    NRF_RADIO->SHORTS = 0;
}

int main(void)
{
    const struct device *console_dev = DEVICE_DT_GET(CONSOLE_DEVICE_LABEL);
    uint32_t dtr = 0;

    usb_enable(NULL);
    
    // 如果你想看 Log，可以取消下面註解等待 Serial 連接
    // while (!dtr) {
    //     uart_line_ctrl_get(console_dev, UART_LINE_CTRL_DTR, &dtr);
    //     k_sleep(K_MSEC(100));
    // }

    printk("\n>>> ALARM CLOCK STARTED: Waking up 0xC0552C6A1E <<<\n");

    // 🛠️ 準備喚醒指令 (根據 Ghidra 逆向結果)
    // 邏輯：讀取 buffer[1] 的 bit 0
    tx_packet[0] = 0x00; // Byte 0 (忽略或類型)
    tx_packet[1] = 0x01; // Byte 1 (Bit 0 = 1 -> Wake Up!) <--- 關鍵指令
    tx_packet[2] = 0x00;
    // ... 後面全部補零
    for(int i=3; i<32; i++) tx_packet[i] = 0x00;

    NRF_RADIO->PACKETPTR = (uint32_t)tx_packet;

    int ch_idx = 0;

    while (1) {
        // A. 設定當前頻率
        radio_tx_setup(target_channels[ch_idx]);

        // B. 啟動發射器 (TX Enable)
        NRF_RADIO->EVENTS_READY = 0;
        NRF_RADIO->TASKS_TXEN = 1;
        while(NRF_RADIO->EVENTS_READY == 0);

        // C. 發射封包！ (Fire!)
        NRF_RADIO->EVENTS_END = 0;
        NRF_RADIO->TASKS_START = 1;
        while(NRF_RADIO->EVENTS_END == 0);

        // D. 關閉無線電 (必須先關閉才能換頻率)
        NRF_RADIO->TASKS_DISABLE = 1;
        
        // printk("Ping sent to Ch:%d\n", target_channels[ch_idx]);

        // E. 快速切換下一個頻率
        // 我們要製造「彈幕」，讓 Tracker 無論跳到哪個頻道都能被打中
        // 5ms 切換一次
        k_busy_wait(5000); 
        
        ch_idx++;
        if (ch_idx >= CH_COUNT) ch_idx = 0;
    }
}

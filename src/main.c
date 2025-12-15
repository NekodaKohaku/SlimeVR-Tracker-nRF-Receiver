/*
 * Copyright (c) 2025 White Cat DIY
 * Pico Tracker Waker - "The Alarm Clock"
 * Target Address: 0xC0552C6A1E
 * Payload: 0x00 0x01 (Wake Up Command)
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

// 📡 攻擊頻率表 (根據之前的觀測)
// 我們輪流在這幾個頻率發射，確保 Tracker 跳到哪都能聽到
static const int target_channels[] = {1, 37, 77, 40}; // 加一個 40 (2440MHz) 保險
#define CH_COUNT 4

// 📦 封包緩衝區
static uint8_t tx_packet[32];

void radio_tx_setup(int channel)
{
    NRF_RADIO->TASKS_DISABLE = 1;
    while (NRF_RADIO->EVENTS_DISABLED == 0);
    NRF_RADIO->EVENTS_DISABLED = 0;

    NRF_RADIO->FREQUENCY = channel;
    
    // 設定 2Mbit ESB 模式
    NRF_RADIO->MODE = RADIO_MODE_MODE_Nrf_2Mbit; 

    // 設定地址
    NRF_RADIO->BASE0 = TARGET_BASE_ADDR;
    NRF_RADIO->PREFIX0 = TARGET_PREFIX;
    NRF_RADIO->TXADDRESS = 0; // 使用 Logical Address 0 發射
    NRF_RADIO->RXADDRESSES = 1;

    // 設定封包格式 (標準 ESB)
    // S0, S1, Length 都不用，我們直接發 Payload
    NRF_RADIO->PCNF0 = 0;
    
    // MaxLen=32, Balen=4
    NRF_RADIO->PCNF1 = (32 << RADIO_PCNF1_MAXLEN_Pos) | 
                       (4 << RADIO_PCNF1_BALEN_Pos) | 
                       (RADIO_PCNF1_ENDIAN_Little << RADIO_PCNF1_ENDIAN_Pos) |
                       (0 << RADIO_PCNF1_WHITEEN_Pos); // 暫時不開白化

    // CRC (一定要開，不然 Tracker 會認為是雜訊丟掉)
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
    // 等待 USB 連接 (非必須，但方便看 Log)
    // while (!dtr) {
    //     uart_line_ctrl_get(console_dev, UART_LINE_CTRL_DTR, &dtr);
    //     k_sleep(K_MSEC(100));
    // }

    printk("\n>>> ALARM CLOCK STARTED: Waking up 0xC0552C6A1E <<<\n");

    // 🛠️ 準備喚醒指令 (根據 Ghidra 逆向結果)
    // 0x20000AA5 是 Base，程式讀取 Base+1 (0x20000AA6) 的 Bit 0
    tx_packet[0] = 0x00; // Byte 0 (忽略)
    tx_packet[1] = 0x01; // Byte 1 (Bit 0 = 1 -> Wake Up!)
    tx_packet[2] = 0x00;
    // ... 後面補零

    NRF_RADIO->PACKETPTR = (uint32_t)tx_packet;

    int ch_idx = 0;

    while (1) {
        // 1. 設定頻率
        radio_tx_setup(target_channels[ch_idx]);

        // 2. 啟動發射器 (TX Enable)
        NRF_RADIO->EVENTS_READY = 0;
        NRF_RADIO->TASKS_TXEN = 1;
        while(NRF_RADIO->EVENTS_READY == 0);

        // 3. 發射封包！ (Fire!)
        NRF_RADIO->EVENTS_END = 0;
        NRF_RADIO->TASKS_START = 1;
        while(NRF_RADIO->EVENTS_END == 0);

        // 4. 關閉無線電 (休息一下)
        NRF_RADIO->TASKS_DISABLE = 1;
        
        printk("Sent WakeUp to Ch:%d\n", target_channels[ch_idx]);

        // 5. 切換下一個頻率
        // 我們要發快一點，增加 Tracker 剛好跳到該頻率撞見我們的機率
        k_busy_wait(5000); // 等待 5ms
        
        ch_idx++;
        if (ch_idx >= CH_COUNT) ch_idx = 0;
    }
}

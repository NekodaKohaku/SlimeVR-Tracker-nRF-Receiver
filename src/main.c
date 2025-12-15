/*
 * Copyright (c) 2025 White Cat DIY
 * Pico Tracker Scanner - "The Blind Sniffer"
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <hal/nrf_radio.h>
#include <zephyr/usb/usb_device.h>
#include <zephyr/drivers/uart.h>

#define CONSOLE_DEVICE_LABEL DT_CHOSEN(zephyr_console)

// 這是標準 BLE 廣播地址，配對模式通常用這個
#define BLE_ACCESS_ADDRESS 0x8E89BED6 

void radio_configure(int channel)
{
    NRF_RADIO->TASKS_DISABLE = 1;
    while (NRF_RADIO->EVENTS_DISABLED == 0);
    NRF_RADIO->EVENTS_DISABLED = 0;

    // 1. 設定速率 (建議先試 Nrf_2Mbit，如果沒反應再改 Nrf_1Mbit)
    NRF_RADIO->MODE = RADIO_MODE_MODE_Nrf_2Mbit; 

    // 2. 設定頻率
    NRF_RADIO->FREQUENCY = channel;

    // 3. 設定邏輯地址 (Logical Address)
    // 我們設定 Base0 + Prefix0
    NRF_RADIO->BASE0 = (BLE_ACCESS_ADDRESS << 8); 
    NRF_RADIO->PREFIX0 = (BLE_ACCESS_ADDRESS >> 24) & 0xFF;
    
    // 使用邏輯地址 0
    NRF_RADIO->TXADDRESS = 0;
    NRF_RADIO->RXADDRESSES = 1;

    // 4. 設定封包格式 (这是关键！我们要设得宽松一点)
    // S0, S1, L 长度设为标准，但也可能要根据私有协议调整
    NRF_RADIO->PCNF0 = (0 << RADIO_PCNF0_S1LEN_Pos) |
                       (0 << RADIO_PCNF0_S0LEN_Pos) |
                       (8 << RADIO_PCNF0_LFLEN_Pos); 

    // Payload 最大 255 bytes, 不啟用 Whitening (先關掉以排除變數)
    NRF_RADIO->PCNF1 = (255 << RADIO_PCNF1_MAXLEN_Pos) |
                       (0 << RADIO_PCNF1_STATLEN_Pos) |
                       (3 << RADIO_PCNF1_BALEN_Pos) | // Base address length 3 + Prefix 1 = 4 bytes
                       (RADIO_PCNF1_ENDIAN_Little << RADIO_PCNF1_ENDIAN_Pos) |
                       (0 << RADIO_PCNF1_WHITEEN_Pos); // 關閉白化

    // 5. 關閉 CRC (為了看到爛封包)
    NRF_RADIO->CRCCNF = 0; 
}

int main(void)
{
    const struct device *console_dev = DEVICE_DT_GET(CONSOLE_DEVICE_LABEL);
    uint32_t dtr = 0;

    usb_enable(NULL);
    while (!dtr) {
        uart_line_ctrl_get(console_dev, UART_LINE_CTRL_DTR, &dtr);
        k_sleep(K_MSEC(100));
    }

    printk("\n>>> BLIND SNIFFER STARTED (Target: 2440 MHz) <<<\n");

    // 設定為你發現強訊號的那個頻道 (例如 40)
    int target_ch = 40; 
    radio_configure(target_ch);

    // 準備接收 Buffer
    static uint8_t rx_buffer[256];
    NRF_RADIO->PACKETPTR = (uint32_t)rx_buffer;

    while (1) {
        // 啟動接收
        NRF_RADIO->EVENTS_READY = 0;
        NRF_RADIO->TASKS_RXEN = 1;
        while (NRF_RADIO->EVENTS_READY == 0);
        
        NRF_RADIO->EVENTS_END = 0;
        NRF_RADIO->TASKS_START = 1;

        // 等待收到封包 或 超時
        // 這裡我們用一個簡單的計數器當 timeout
        int timeout = 100000;
        while (NRF_RADIO->EVENTS_END == 0 && timeout > 0) {
            timeout--;
        }

        if (NRF_RADIO->EVENTS_END) {
            NRF_RADIO->EVENTS_END = 0;
            NRF_RADIO->TASKS_STOP = 1;
            
            // 如果收到了，把資料印出來！
            // 檢查 RSSI 確保不是雜訊
            if (NRF_RADIO->RSSISAMPLE > 50) { // 這裡 RSSI 是正數 (0~127)
                 printk("PKT! RSSI:-%d Data: ", NRF_RADIO->RSSISAMPLE);
                 for(int i=0; i<10; i++) { // 先印前 10 個 byte 看看
                     printk("%02X ", rx_buffer[i]);
                 }
                 printk("\n");
            }
        } else {
            // Timeout，重來
            NRF_RADIO->TASKS_STOP = 1;
        }
        
        k_sleep(K_MSEC(10));
    }
}

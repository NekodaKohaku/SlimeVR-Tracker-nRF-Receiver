/*
 * Copyright (c) 2025 White Cat DIY
 * Pico Tracker Scanner - "The Geiger Counter" (Pure Energy Detector)
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <hal/nrf_radio.h>
#include <zephyr/usb/usb_device.h>
#include <zephyr/drivers/uart.h>

#define CONSOLE_DEVICE_LABEL DT_CHOSEN(zephyr_console)

// 直接操作底層暫存器，繞過所有協議檢查
void raw_radio_setup(void)
{
    // 1. 關閉 Radio 確保安全
    NRF_RADIO->TASKS_DISABLE = 1;
    while (NRF_RADIO->EVENTS_DISABLED == 0);
    NRF_RADIO->EVENTS_DISABLED = 0;

    // 2. 設定為 1Mbit 模式 (頻譜較寬，最容易捕捉到能量)
    NRF_RADIO->MODE = RADIO_MODE_MODE_Nrf_1Mbit;

    // 3. 清除所有自動化捷徑
    NRF_RADIO->SHORTS = 0;
    
    // 4. 設定預設頻率 (之後會動態改)
    NRF_RADIO->FREQUENCY = 40; 
}

int main(void)
{
    const struct device *console_dev = DEVICE_DT_GET(CONSOLE_DEVICE_LABEL);
    uint32_t dtr = 0;

    // 啟動 USB Console
    usb_enable(NULL);
    
    // 等待電腦連線 (避免錯過 Log)
    while (!dtr) {
        uart_line_ctrl_get(console_dev, UART_LINE_CTRL_DTR, &dtr);
        k_sleep(K_MSEC(100));
    }

    printk("\n>>> GEIGER COUNTER READY (Scanning 2440 MHz area) <<<\n");
    printk(">>> Bypassing Address Check... Measuring Pure RF Energy.\n");

    raw_radio_setup();

    // 掃描範圍：2438 MHz ~ 2442 MHz (Channel 38 - 42)
    // 稍微掃描周圍，以免晶振頻率飄移
    int start_ch = 38;
    int end_ch = 42;

    while (1) {
        for (int ch = start_ch; ch <= end_ch; ch++) {
            
            // 1. 切換頻率前先停機
            NRF_RADIO->TASKS_DISABLE = 1; 
            while (NRF_RADIO->EVENTS_DISABLED == 0); 
            NRF_RADIO->EVENTS_DISABLED = 0;
            
            NRF_RADIO->FREQUENCY = ch;

            // 2. 開啟接收 (RXEN)
            NRF_RADIO->EVENTS_READY = 0;
            NRF_RADIO->TASKS_RXEN = 1;
            while (NRF_RADIO->EVENTS_READY == 0); // 等待 Radio 暖機 (約 130us)
            NRF_RADIO->EVENTS_READY = 0;

            // 3. === 關鍵 === 直接啟動 RSSI 測量，不等待封包！
            NRF_RADIO->EVENTS_RSSIEND = 0;
            NRF_RADIO->TASKS_RSSISTART = 1;
            
            // 等待測量完成
            while (NRF_RADIO->EVENTS_RSSIEND == 0);
            NRF_RADIO->EVENTS_RSSIEND = 0;

            // 4. 讀取數值 (數值是負的 dBm，但在暫存器裡是正數)
            uint8_t sample = NRF_RADIO->RSSISAMPLE;
            int rssi = -1 * (int)sample;

            // 5. 判斷與顯示
            // -90 dBm 左右是空氣雜訊 (沒訊號)
            // > -70 dBm 代表有強訊號
            if (rssi > -75) {
                printk("[Hit!] %d MHz | RSSI: %d dBm <--- TARGET DETECTED!\n", 2400 + ch, rssi);
            } 
            // 如果你想看它是活著的，可以取消下面這行的註解，但會洗版
            // else { printk("."); } 
            
            // 稍微停一下
            k_busy_wait(2000); 
        }
        // 換行分隔
        // printk("\n");
    }
}

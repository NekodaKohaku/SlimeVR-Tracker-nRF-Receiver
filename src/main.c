/*
 * Copyright (c) 2025 White Cat DIY
 * Pico Tracker Scanner - "The Sniper" (Fixed 2440MHz)
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <hal/nrf_radio.h>
#include <zephyr/usb/usb_device.h>
#include <zephyr/drivers/uart.h>

// 定義 Console
#define CONSOLE_DEVICE_LABEL DT_CHOSEN(zephyr_console)

void radio_configure(void)
{
    // 1. 先停用 Radio
    nrf_radio_task_trigger(NRF_RADIO, NRF_RADIO_TASK_DISABLE);
    
    // 2. 設定模式：必須跟我們用 pyocd 設定的一樣 (Nrf_2Mbit)
    // 注意：這裡不能用 BLE_2MBIT，要用 NRF_2MBIT (私有協議模式)
    nrf_radio_mode_set(NRF_RADIO, NRF_RADIO_MODE_NRF_2MBIT);
    
    // 3. 設定發射功率 (雖然我們只接收，但設定一下也好)
    nrf_radio_txpower_set(NRF_RADIO, NRF_RADIO_TXPOWER_0DBM);
}

int main(void)
{
    const struct device *console_dev = DEVICE_DT_GET(CONSOLE_DEVICE_LABEL);
    uint32_t dtr = 0;

    // 啟動 USB Console
    usb_enable(NULL);
    
    // 等待連線 (讓你不會錯過 Log)
    while (!dtr) {
        uart_line_ctrl_get(console_dev, UART_LINE_CTRL_DTR, &dtr);
        k_sleep(K_MSEC(100));
    }

    printk("\n>>> SNIPER SCANNER READY (Target: 2440 MHz) <<<\n");

    radio_configure();

    while (1) {
        // === 關鍵設定：鎖定 2440 MHz ===
        // Channel 40 = 2440 MHz
        nrf_radio_frequency_set(NRF_RADIO, 40); 
        
        // 啟動接收
        nrf_radio_task_trigger(NRF_RADIO, NRF_RADIO_TASK_RXEN);
        k_busy_wait(150); // 等待 Radio 暖機
        
        // 啟動 RSSI 測量
        nrf_radio_task_trigger(NRF_RADIO, NRF_RADIO_TASK_RSSISTART);
        k_busy_wait(500); // 採樣時間

        // 讀取數值
        uint8_t sample = nrf_radio_rssi_sample_get(NRF_RADIO);
        int rssi = -1 * (int)sample;

        // 停止 RSSI
        nrf_radio_task_trigger(NRF_RADIO, NRF_RADIO_TASK_RSSISTOP);
        nrf_radio_task_trigger(NRF_RADIO, NRF_RADIO_TASK_DISABLE);

        // 過濾雜訊：只有訊號夠強才印出來
        // 平常雜訊大約 -90 到 -100
        // 如果 pyocd 成功，應該會看到 -30 ~ -50
        if (rssi > -80) {
            printk("!!! DETECTED !!! Freq: 2440 MHz | RSSI: %d dBm\n", rssi);
        } else {
            // 每 10 次印一次心跳，證明 Scanner 活著
            static int count = 0;
            if (count++ % 10 == 0) {
                printk("Scanning 2440MHz... (Current Noise: %d dBm)\n", rssi);
            }
        }
        
        k_sleep(K_MSEC(50)); // 快速掃描
    }
}

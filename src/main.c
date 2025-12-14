/*
 * Copyright (c) 2025 White Cat DIY
 * Pico Tracker Scanner - "The Net" (Sweeping Mode)
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <hal/nrf_radio.h>
#include <zephyr/usb/usb_device.h>
#include <zephyr/drivers/uart.h>

#define CONSOLE_DEVICE_LABEL DT_CHOSEN(zephyr_console)

void radio_configure(void)
{
    nrf_radio_task_trigger(NRF_RADIO, NRF_RADIO_TASK_DISABLE);
    
    // === 改動 1：使用最標準的 BLE 1Mbit 模式 ===
    nrf_radio_mode_set(NRF_RADIO, NRF_RADIO_MODE_BLE_1MBIT);
    
    nrf_radio_txpower_set(NRF_RADIO, NRF_RADIO_TXPOWER_0DBM);
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

    printk("\n>>> SCANNER: SWEEPING 2438-2442 MHz (BLE 1M) <<<\n");

    radio_configure();

    while (1) {
        // === 改動 2：在 2440MHz 附近掃描 (CH 38 ~ CH 42) ===
        for (int ch = 38; ch <= 42; ch++) { 
            nrf_radio_frequency_set(NRF_RADIO, ch); 
            
            nrf_radio_task_trigger(NRF_RADIO, NRF_RADIO_TASK_RXEN);
            k_busy_wait(40); // 縮短等待時間
            
            nrf_radio_task_trigger(NRF_RADIO, NRF_RADIO_TASK_RSSISTART);
            k_busy_wait(100); // 取樣

            uint8_t sample = nrf_radio_rssi_sample_get(NRF_RADIO);
            int rssi = -1 * (int)sample;

            nrf_radio_task_trigger(NRF_RADIO, NRF_RADIO_TASK_RSSISTOP);
            nrf_radio_task_trigger(NRF_RADIO, NRF_RADIO_TASK_DISABLE);

            // 只要訊號大於 -75 就叫出來 (過濾雜訊)
            if (rssi > -75) {
                printk("!!! FOUND !!! Freq: %d MHz | RSSI: %d dBm\n", 2400 + ch, rssi);
            }
        }
        // 稍微休息一下，讓 Log 不要太快
        k_sleep(K_MSEC(50));
    }
}

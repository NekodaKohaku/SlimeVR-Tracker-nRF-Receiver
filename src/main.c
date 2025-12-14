/*
 * Copyright (c) 2025 White Cat DIY
 * Pico Tracker Reverse Engineering - Frequency Sweeper (Fixed USB)
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <hal/nrf_radio.h>
#include <zephyr/usb/usb_device.h>  // [新增] USB 驅動
#include <zephyr/drivers/uart.h>    // [新增] UART 驅動

// 設定掃描參數
#define SCAN_DELAY_MS 20       
#define RSSI_THRESHOLD -50     

// 直接操作硬體暫存器
void radio_configure(void)
{
    nrf_radio_task_trigger(NRF_RADIO, NRF_RADIO_TASK_DISABLE);
    
    // 這裡我們維持 2Mbit，因為這對應到大多數高效能 Tracker
    nrf_radio_mode_set(NRF_RADIO, NRF_RADIO_MODE_BLE_2MBIT);

    nrf_radio_txpower_set(NRF_RADIO, NRF_RADIO_TXPOWER_0DBM);
    nrf_radio_modecnf0_set(NRF_RADIO, true, 0);
}

int main(void)
{
    // === [新增] USB 初始化與等待 ===
    int ret = usb_enable(NULL);
    if (ret != 0) {
        return 0; // USB 啟動失敗
    }

    // 等待 3 秒鐘讓電腦識別 USB 裝置，並給你時間打開 Terminal
    k_sleep(K_SECONDS(3)); 
    // ============================

    printk("\n\n");
    printk("============================================\n");
    printk("*** Pico Universal Frequency Sweeper v1.1 ***\n");
    printk("============================================\n");
    printk("Scanning 2402 MHz - 2480 MHz for STRONG signals ( > %d dBm)...\n", RSSI_THRESHOLD);
    printk("Please place the Pico Tracker VERY CLOSE to the antenna!\n");

    radio_configure();

    while (1) {
        // 頻率迴圈：從 2402 (CH 2) 掃到 2480 (CH 80)
        for (int freq = 2; freq <= 80; freq++) {
            
            nrf_radio_frequency_set(NRF_RADIO, freq);
            nrf_radio_task_trigger(NRF_RADIO, NRF_RADIO_TASK_RXEN);
            k_busy_wait(150); 

            nrf_radio_task_trigger(NRF_RADIO, NRF_RADIO_TASK_RSSISTART);
            k_busy_wait(50);

            uint8_t sample = nrf_radio_rssi_sample_get(NRF_RADIO);
            int rssi = -1 * (int)sample;

            // 判斷並顯示
            if (rssi > RSSI_THRESHOLD) {
                printk(">>> [HIT] Freq: 24%02d MHz | RSSI: %d dBm <<<\n", freq, rssi);
            }

            nrf_radio_task_trigger(NRF_RADIO, NRF_RADIO_TASK_RSSISTOP);
            nrf_radio_task_trigger(NRF_RADIO, NRF_RADIO_TASK_DISABLE);
            
            k_msleep(SCAN_DELAY_MS);
        }
        
        // 為了證明程式還活著，每掃描完一輪閃爍一下 LED (如果你有的話) 或印個點
        // printk("."); 
    }
}

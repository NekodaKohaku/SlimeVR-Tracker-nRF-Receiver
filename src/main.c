/*
 * Copyright (c) 2025 White Cat DIY
 * Pico Tracker Reverse Engineering - Frequency Sweeper
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <hal/nrf_radio.h>

// 設定掃描參數
#define SCAN_DELAY_MS 20       // 每個頻道的停留時間 (毫秒)
#define RSSI_THRESHOLD -50     // 訊號過濾門檻 (只顯示比這強的訊號)

// 直接操作硬體暫存器，速度最快
void radio_configure(void)
{
    // 1. 關閉 Radio 以便設定
    nrf_radio_task_trigger(NRF_RADIO, NRF_RADIO_TASK_DISABLE);
    
    // 2. 設定為 2Mbit (Pico Tracker 幾乎肯定是 2Mbit)
    // 如果掃不到，可以改回 NRF_RADIO_MODE_BLE_1MBIT 試試
    nrf_radio_mode_set(NRF_RADIO, NRF_RADIO_MODE_BLE_2MBIT);

    // 3. 設定發射功率為 0 dBm (雖然我們只收不發)
    nrf_radio_txpower_set(NRF_RADIO, NRF_RADIO_TXPOWER_0DBM);

    // 4. 設定 Fast Ramp-up
    nrf_radio_modecnf0_set(NRF_RADIO, true, 0);
}

int main(void)
{
    printk("*** Pico Universal Frequency Sweeper Started ***\n");
    printk("Scanning 2402 MHz - 2480 MHz for STRONG signals ( > %d dBm)...\n", RSSI_THRESHOLD);

    radio_configure();

    while (1) {
        // 頻率迴圈：從 2402 (CH 2) 掃到 2480 (CH 80)
        // 這是 2.4GHz ISM 頻段的範圍
        for (int freq = 2; freq <= 80; freq++) {
            
            // 1. 設定頻率 (2400 + freq MHz)
            nrf_radio_frequency_set(NRF_RADIO, freq);

            // 2. 啟動接收 (RX)
            nrf_radio_task_trigger(NRF_RADIO, NRF_RADIO_TASK_RXEN);
            
            // 等待 Radio 準備好 (Ready)
            // 在 nRF52/53 上，RXEN -> READY 需要幾十微秒
            k_busy_wait(150); 

            // 3. 啟動 RSSI 測量
            nrf_radio_task_trigger(NRF_RADIO, NRF_RADIO_TASK_RSSISTART);
            
            // 等待測量完成 (稍微等一下確保採樣準確)
            k_busy_wait(50); // 實際上 RSSI 採樣很快，但給它一點時間捕捉

            // 4. 讀取 RSSI 數值
            // nrf_radio_rssi_sample_get() 回傳的是正整數 (例如 60 代表 -60dBm)
            uint8_t sample = nrf_radio_rssi_sample_get(NRF_RADIO);
            int rssi = -1 * (int)sample;

            // 5. 判斷並顯示
            if (rssi > RSSI_THRESHOLD) {
                printk(">>> [HIT] Freq: 24%02d MHz | RSSI: %d dBm <<<\n", freq, rssi);
            }

            // 6. 停止 RSSI 和 RX，準備切換下一個頻率
            nrf_radio_task_trigger(NRF_RADIO, NRF_RADIO_TASK_RSSISTOP);
            nrf_radio_task_trigger(NRF_RADIO, NRF_RADIO_TASK_DISABLE);
            
            // 讓 CPU 休息一下，避免 Watchdog 叫，也能控制掃描速度
            k_msleep(SCAN_DELAY_MS);
        }
        
        // 掃完一輪印個分隔線，方便閱讀
        // printk("--- Loop Complete ---\n");
    }
}

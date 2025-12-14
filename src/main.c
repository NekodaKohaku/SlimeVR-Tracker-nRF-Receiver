/*
 * Copyright (c) 2025 White Cat DIY
 * Pico Tracker Debugger - Clock Fixed Version
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <hal/nrf_radio.h>
#include <hal/nrf_clock.h> // [新增] 需要控制時鐘
#include <zephyr/usb/usb_device.h>
#include <zephyr/drivers/gpio.h>

void clock_init(void)
{
    printk("[2] Starting External Crystal (HFXO)...\n");
    // 啟動高頻晶振任務
    nrf_clock_task_trigger(NRF_CLOCK, NRF_CLOCK_TASK_HFCLKSTART);

    // 等待晶振穩定 (Running)
    int timeout = 1000;
    while (!nrf_clock_event_check(NRF_CLOCK, NRF_CLOCK_EVENT_HFCLKSTARTED) && timeout > 0) {
        k_busy_wait(1000); // 等 1ms
        timeout--;
    }

    if (timeout <= 0) {
        printk("!!! ERROR: Clock start timeout! Radio might fail.\n");
    } else {
        // 清除事件
        nrf_clock_event_clear(NRF_CLOCK, NRF_CLOCK_EVENT_HFCLKSTARTED);
        printk("[3] Clock is stable!\n");
    }
}

void radio_configure(void)
{
    printk("[4] Configuring Radio Registers...\n");
    nrf_radio_task_trigger(NRF_RADIO, NRF_RADIO_TASK_DISABLE);
    
    // 設定為 1Mbit BLE 模式 (測試用)
    nrf_radio_mode_set(NRF_RADIO, NRF_RADIO_MODE_BLE_1MBIT);
    nrf_radio_txpower_set(NRF_RADIO, NRF_RADIO_TXPOWER_0DBM);
    
    // 設定 Fast Ramp-up
    nrf_radio_modecnf0_set(NRF_RADIO, true, 0);
    printk("[5] Radio Configured.\n");
}

int main(void)
{
    if (usb_enable(NULL)) return 0;
    k_sleep(K_SECONDS(3)); 

    printk("\n*** SYSTEM BOOT COMPLETE ***\n");
    printk("[1] Main started. Initializing Clock...\n");

    // 1. 啟動時鐘 (關鍵！)
    clock_init();

    // 2. 設定 Radio
    radio_configure();

    printk("[6] Entering Loop. Scanning 2402 MHz...\n");

    while (1) {
        // 固定在 2402MHz (CH37) 測試
        nrf_radio_frequency_set(NRF_RADIO, 2);
        
        // 啟動接收
        nrf_radio_task_trigger(NRF_RADIO, NRF_RADIO_TASK_RXEN);
        
        // 加一個簡單的 timeout 保護，避免卡死在 Ready
        int safety = 10000;
        while (!nrf_radio_event_check(NRF_RADIO, NRF_RADIO_EVENT_READY) && safety > 0) {
            safety--;
        }
        
        if (safety <= 0) {
             printk("XXX Radio Stuck at RXEN! Check Hardware/Config.\n");
             nrf_radio_task_trigger(NRF_RADIO, NRF_RADIO_TASK_DISABLE);
             k_sleep(K_SECONDS(1));
             continue;
        }

        nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_READY);

        // 啟動 RSSI
        nrf_radio_task_trigger(NRF_RADIO, NRF_RADIO_TASK_RSSISTART);
        k_busy_wait(100); // 等久一點

        uint8_t sample = nrf_radio_rssi_sample_get(NRF_RADIO);
        int rssi = -1 * (int)sample;

        // 印出結果
        printk(">>> Alive! RSSI on 2402MHz: %d dBm\n", rssi);

        // 關閉
        nrf_radio_task_trigger(NRF_RADIO, NRF_RADIO_TASK_RSSISTOP);
        nrf_radio_task_trigger(NRF_RADIO, NRF_RADIO_TASK_DISABLE);
        
        k_msleep(500); 
    }
}

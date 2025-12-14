/*
 * Copyright (c) 2025 White Cat DIY
 * Pico Tracker Debugger - No Filter Mode
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <hal/nrf_radio.h>
#include <zephyr/usb/usb_device.h>
#include <zephyr/drivers/gpio.h>

// 嘗試定義 LED (如果你的板子有 LED0)
// nRF52840 Dongle 的綠燈通常是 led0_green
#define LED0_NODE DT_ALIAS(led0)
static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET_OR(LED0_NODE, gpios, {0});

void radio_configure(void)
{
    nrf_radio_task_trigger(NRF_RADIO, NRF_RADIO_TASK_DISABLE);
    
    // 這次我們改回 1Mbit (BLE標準)，先確定能收到環境雜訊
    // 如果這裡能收到雜訊，代表 Radio 沒壞
    nrf_radio_mode_set(NRF_RADIO, NRF_RADIO_MODE_BLE_1MBIT);

    nrf_radio_txpower_set(NRF_RADIO, NRF_RADIO_TXPOWER_0DBM);
    nrf_radio_modecnf0_set(NRF_RADIO, true, 0);
}

int main(void)
{
    // 初始化 USB
    if (usb_enable(NULL)) {
        return 0; 
    }
    
    // 初始化 LED
    if (gpio_is_ready_dt(&led)) {
        gpio_pin_configure_dt(&led, GPIO_OUTPUT_ACTIVE);
    }

    // 給你 5 秒鐘打開 Terminal
    k_sleep(K_SECONDS(5)); 

    printk("\n*** RADIO DEBUG MODE STARTED ***\n");
    printk("We will print EVERYTHING regardless of signal strength.\n");

    radio_configure();

    while (1) {
        // 我們只掃描 3 個頻道就好，節省時間，先確定 Radio 會動
        // 2402 (CH37), 2440 (CH17), 2480 (CH39)
        int test_freqs[] = {2, 40, 80}; 

        for (int i = 0; i < 3; i++) {
            int freq = test_freqs[i];
            
            nrf_radio_frequency_set(NRF_RADIO, freq);
            nrf_radio_task_trigger(NRF_RADIO, NRF_RADIO_TASK_RXEN);
            k_busy_wait(150); 

            nrf_radio_task_trigger(NRF_RADIO, NRF_RADIO_TASK_RSSISTART);
            k_busy_wait(20);

            uint8_t sample = nrf_radio_rssi_sample_get(NRF_RADIO);
            int rssi = -1 * (int)sample;

            // 無條件列印！
            printk("Freq: 24%02d MHz | RSSI: %d dBm\n", freq, rssi);

            nrf_radio_task_trigger(NRF_RADIO, NRF_RADIO_TASK_RSSISTOP);
            nrf_radio_task_trigger(NRF_RADIO, NRF_RADIO_TASK_DISABLE);
            
            // 閃爍 LED 證明活著
            if (gpio_is_ready_dt(&led)) {
                gpio_pin_toggle_dt(&led);
            }
            
            k_msleep(100); // 慢一點，讓你看清楚
        }
        printk("----------------\n");
        k_msleep(500);
    }
}

/*
 * Pico Tracker HUNTER v15 (RSSI Analyzer)
 * 診斷模式：不解碼，只看有沒有「能量」
 * 用途：確認 Tracker 到底在不在這些頻率上？
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <hal/nrf_radio.h>
#include <zephyr/usb/usb_device.h>
#include <zephyr/drivers/gpio.h>

// 1. 懷疑的頻率
static const uint8_t target_freqs[] = {46, 54, 72, 80}; 

// 2. LED
#define LED0_NODE DT_ALIAS(led0)
static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(LED0_NODE, gpios);

void radio_init(void)
{
    NRF_RADIO->POWER = 0;
    k_busy_wait(500);
    NRF_RADIO->POWER = 1;

    NRF_RADIO->MODE = NRF_RADIO_MODE_BLE_2MBIT;
    
    // 即使只是測 RSSI，基本的 Radio 啟用也需要設定
    // 隨便設個地址，反正我們不等 Packet，我們只看能量
    NRF_RADIO->BASE0 = 0x552C6A1E;
    NRF_RADIO->PREFIX0 = 0xC0;
    NRF_RADIO->RXADDRESSES = 1; 

    // 捷徑：RSSI 測量完自動停止
    NRF_RADIO->SHORTS = RADIO_SHORTS_READY_START_Msk; 
}

int main(void)
{
    if (device_is_ready(led.port)) {
        gpio_pin_configure_dt(&led, GPIO_OUTPUT_ACTIVE);
        gpio_pin_set_dt(&led, 0); 
    }

    usb_enable(NULL);
    
    // 延遲啟動
    for(int i=0; i<6; i++) {
        gpio_pin_toggle_dt(&led);
        k_sleep(K_MSEC(500));
    }
    gpio_pin_set_dt(&led, 0); 

    printk("\n\n");
    printk("============================================\n");
    printk(">>> HUNTER v15 (Signal Analyzer)         <<<\n");
    printk(">>> Testing Signal Strength (RSSI)       <<<\n");
    printk(">>> Put Tracker VERY CLOSE to Dongle!    <<<\n");
    printk("============================================\n");

    radio_init();

    int freq_idx = 0;

    while (1) {
        int current_freq = target_freqs[freq_idx];
        NRF_RADIO->FREQUENCY = current_freq;
        
        // 啟動接收
        NRF_RADIO->EVENTS_READY = 0;
        NRF_RADIO->TASKS_RXEN = 1;
        while (NRF_RADIO->EVENTS_READY == 0); // 等待 Radio 暖機完成

        // 啟動 RSSI 採樣
        NRF_RADIO->EVENTS_RSSIEND = 0;
        NRF_RADIO->TASKS_RSSISTART = 1;
        
        // 等待採樣完成
        while (NRF_RADIO->EVENTS_RSSIEND == 0);

        // 讀取 RSSI (數值是負的 dBm)
        int8_t rssi = -(int8_t)NRF_RADIO->RSSISAMPLE;

        // 關閉 Radio (省電並重置狀態)
        NRF_RADIO->TASKS_DISABLE = 1;
        while (NRF_RADIO->EVENTS_DISABLED == 0);

        // ★ 判斷訊號強度 ★
        // 一般雜訊大約在 -90 到 -100 dBm
        // 如果 Tracker 在旁邊，訊號應該要在 -60 dBm 以上 (數字越大越強，例如 -40)
        
        if (rssi > -70) { 
            // 訊號強！這裡有東西！
            gpio_pin_toggle_dt(&led);
            printk(">>> [FOUND] Freq %d MHz | Signal: %d dBm (STRONG!)\n", 2400 + current_freq, rssi);
        } else {
            // 沒訊號，印個點就好
            // printk("."); 
        }

        // 稍微延遲，換下一個頻率
        k_sleep(K_MSEC(10)); // 快速掃描

        freq_idx++;
        if (freq_idx >= sizeof(target_freqs) / sizeof(target_freqs[0])) {
            freq_idx = 0;
            // 掃完一輪如果都沒訊號，印一行分隔線
            // printk("\n");
        }
    }
}

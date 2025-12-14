/*
 * Pico Energy Scanner (RSSI) - FIXED
 * 不管地址，不管內容，只看「能量」。
 * 用來確認硬體通訊層是否建立。
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <hal/nrf_radio.h>
#include <zephyr/usb/usb_device.h>
#include <zephyr/drivers/uart.h>

// 我們鎖定的三個嫌疑頻道
static uint8_t target_channels[] = {1, 37, 77};
static int ch_index = 0;

void radio_init(uint8_t channel) {
    NRF_RADIO->TASKS_DISABLE = 1;
    while (NRF_RADIO->EVENTS_DISABLED == 0);
    NRF_RADIO->EVENTS_DISABLED = 0;
    
    nrf_radio_mode_set(NRF_RADIO, NRF_RADIO_MODE_NRF_2MBIT);
    nrf_radio_frequency_set(NRF_RADIO, channel);
    
    // 啟用接收
    NRF_RADIO->SHORTS = RADIO_SHORTS_READY_START_Msk;
    NRF_RADIO->TASKS_RXEN = 1;
}

int main(void) {
    if (usb_enable(NULL)) return 0;
    
    const struct device *dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_console));
    uint32_t dtr = 0;
    while (!dtr) {
        uart_line_ctrl_get(dev, UART_LINE_CTRL_DTR, &dtr);
        k_sleep(K_MSEC(100));
    }
    
    printk("\n=== Pico Energy/RSSI Scanner ===\n");
    printk("Looking for pulses on CH 1, 37, 77...\n");

    while (1) {
        uint8_t ch = target_channels[ch_index];
        radio_init(ch);
        
        // 在每個頻道聽 200ms
        int high_energy_count = 0;
        
        for(int i=0; i<200; i++) {
            // 啟動 RSSI 測量
            NRF_RADIO->TASKS_RSSISTART = 1;
            k_busy_wait(50); // 等待採樣
            
            if (NRF_RADIO->EVENTS_RSSIEND) {
                NRF_RADIO->EVENTS_RSSIEND = 0;
                
                // [修正點] 這裡是 RSSISAMPLE，不是 RSSI
                uint8_t rssi_val = NRF_RADIO->RSSISAMPLE; 
                
                // 門檻：通常背景雜訊約 90-100dBm (數值越大越弱，這裡是絕對值)
                // 如果 < 60dBm，代表有很強的訊號在附近
                if (rssi_val < 60) {
                    high_energy_count++;
                }
            }
            k_busy_wait(1000); // 1ms
        }
        
        // 如果這個頻道偵測到多次能量脈衝
        if (high_energy_count > 5) {
            printk("[!!!] High Energy Detected on CH %d (Count: %d) [!!!]\n", ch, high_energy_count);
        }

        ch_index++;
        if (ch_index >= 3) ch_index = 0;
    }
}

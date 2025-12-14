/*
 * Pico Visual Spectrum Analyzer
 * 掃描 Channel 0 到 80，並畫出能量圖。
 * 用來「看」出追蹤器躲在哪個頻道。
 */
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <hal/nrf_radio.h>
#include <zephyr/usb/usb_device.h>
#include <zephyr/drivers/uart.h>

void radio_prep(uint8_t channel) {
    NRF_RADIO->TASKS_DISABLE = 1;
    while (NRF_RADIO->EVENTS_DISABLED == 0);
    NRF_RADIO->EVENTS_DISABLED = 0;
    
    nrf_radio_mode_set(NRF_RADIO, NRF_RADIO_MODE_NRF_2MBIT);
    nrf_radio_frequency_set(NRF_RADIO, channel);
    
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
    
    printk("\n=== Visual Spectrum Analyzer ===\n");
    printk("Scanning CH 0-80... Look for the SPIKE when you turn on the tracker!\n");

    while (1) {
        // 快速掃描所有頻道
        for (int ch = 0; ch <= 80; ch++) {
            radio_prep(ch);
            
            // 採樣 RSSI
            NRF_RADIO->TASKS_RSSISTART = 1;
            k_busy_wait(100); 
            
            if (NRF_RADIO->EVENTS_RSSIEND) {
                NRF_RADIO->EVENTS_RSSIEND = 0;
                uint8_t rssi = NRF_RADIO->RSSISAMPLE; // 絕對值 (e.g., 50 means -50dBm)
                
                // 過濾掉微弱雜訊，只顯示強訊號 (< 70dBm)
                if (rssi < 70) {
                    // 畫出長條圖
                    printk("CH %d: ", ch);
                    
                    // 訊號越強 (rssi 越小)，柱子越長
                    int bars = (70 - rssi) / 2; 
                    for (int b=0; b<bars; b++) printk("|");
                    
                    printk(" (-%d dBm)\n", rssi);
                }
            }
        }
        // 掃完一輪休息一下，方便閱讀
        // printk("------------------------------------------------\n"); 
        k_busy_wait(100000); // 100ms
    }
}

/*
 * Pico Energy Check (Hardware Sanity Test)
 * 確認追蹤器發射電路是否正常運作
 */
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <hal/nrf_radio.h>
#include <zephyr/usb/usb_device.h>
#include <zephyr/drivers/uart.h>

void radio_init(void) {
    NRF_RADIO->TASKS_DISABLE = 1;
    while (NRF_RADIO->EVENTS_DISABLED == 0);
    NRF_RADIO->EVENTS_DISABLED = 0;
    
    nrf_radio_mode_set(NRF_RADIO, NRF_RADIO_MODE_NRF_2MBIT);
    nrf_radio_frequency_set(NRF_RADIO, 77); // 鎖定 Channel 77
    
    // 隨便設個地址，反正我們靠 RSSI 判斷
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
    
    printk("\n=== Energy/RSSI Monitor (CH 77) ===\n");
    printk("Turn on your tracker and place it NEAR the dongle.\n");

    radio_init();

    while (1) {
        // 啟動 RSSI 測量
        NRF_RADIO->TASKS_RSSISTART = 1;
        k_busy_wait(100); 
        
        if (NRF_RADIO->EVENTS_RSSIEND) {
            NRF_RADIO->EVENTS_RSSIEND = 0;
            uint8_t rssi = NRF_RADIO->RSSI; 
            
            // 數值越小，訊號越強 (例如 40 比 80 強)
            // 如果近距離，通常會 < 50
            if (rssi < 55) { 
                printk(">>> ENERGY DETECTED! RSSI: -%d dBm <<<\n", rssi);
            }
        }
        // 快速採樣
        k_busy_wait(5000); 
    }
}

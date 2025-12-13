/*
 * Pico Tracker "Spectrum Analyzer"
 * Purpose: Find the transmitting frequency by measuring signal strength (RSSI)
 * Method: Sweep all 2.4GHz frequencies, print only STRONG signals (> -50dBm)
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/usb/usb_device.h>
#include <hal/nrf_radio.h>

void radio_init_rssi(uint8_t freq) {
    // 1. Reset Radio
    NRF_RADIO->TASKS_DISABLE = 1;
    while (NRF_RADIO->EVENTS_DISABLED == 0);
    NRF_RADIO->EVENTS_DISABLED = 0;

    // 2. Power ON
    nrf_radio_power_set(NRF_RADIO, true);
    
    // 3. Set Frequency (0-100, mapping to 2400+N MHz)
    nrf_radio_frequency_set(NRF_RADIO, freq);
    
    // 4. Mode: 1Mbps (Doesn't matter for RSSI, but needed to start RX)
    nrf_radio_mode_set(NRF_RADIO, NRF_RADIO_MODE_BLE_1MBIT);

    // 5. Shortcuts: Ready -> Start
    NRF_RADIO->SHORTS = RADIO_SHORTS_READY_START_Msk;
    
    // 6. Start RX
    NRF_RADIO->TASKS_RXEN = 1;
    while (NRF_RADIO->EVENTS_STARTED == 0); // Wait for radio to start
    NRF_RADIO->EVENTS_STARTED = 0;
}

int8_t get_rssi(void) {
    // Start RSSI measurement
    NRF_RADIO->TASKS_RSSISTART = 1;
    while (NRF_RADIO->EVENTS_RSSIEND == 0); // Wait for measurement
    NRF_RADIO->EVENTS_RSSIEND = 0;
    
    // Read value (value is negative dBm, but register stores positive)
    uint8_t rssi_sample = NRF_RADIO->RSSISAMPLE;
    
    // Stop RSSI
    NRF_RADIO->TASKS_RSSISTOP = 1;
    
    return -((int8_t)rssi_sample);
}

int main(void) {
    // Enable USB
    if (IS_ENABLED(CONFIG_USB_DEVICE_STACK)) {
        usb_enable(NULL);
    }
    
    k_sleep(K_SECONDS(2));
    printk("=== Pico Spectrum Analyzer ===\n");
    printk("Searching for STRONG signals (> -50 dBm)...\n");
    printk("Please hold the tracker VERY close to the dongle.\n");

    while (1) {
        // Sweep standard BLE Advertising Channels FIRST
        // Ch37=2402, Ch38=2426, Ch39=2480
        uint8_t target_freqs[] = {2, 26, 80}; 
        
        for (int i = 0; i < 3; i++) {
            uint8_t f = target_freqs[i];
            
            radio_init_rssi(f);
            k_busy_wait(100); // Wait stable
            
            int8_t rssi = get_rssi();
            
            // 如果訊號很強 (大於 -50dBm)，代表就在旁邊！
            if (rssi > -55) {
                printk("\n[DETECTED] Freq: 24%02d MHz | RSSI: %d dBm", f, rssi);
                if (f == 2) printk(" (BLE CH 37)");
                if (f == 26) printk(" (BLE CH 38)");
                if (f == 80) printk(" (BLE CH 39)");
                printk("\n");
                
                // 抓到強訊號後，快速多測幾次確認
                for(int k=0; k<10; k++) {
                    printk("*");
                    k_busy_wait(50000); // 50ms
                }
            }
        }
        
        // 為了不洗版，沒訊號時我們沉默，或者印個點
        // printk("."); 
        k_sleep(K_MSEC(10));
    }
    return 0;
}

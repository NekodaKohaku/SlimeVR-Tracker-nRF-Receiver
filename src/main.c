/*
 * Pico Sniper Ch37 (Noise Immune)
 * Strategy: Ignore Ch 1/77. Lock onto Ch 37.
 * Dwell time: Increased to 100ms per prefix to catch the blink.
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <hal/nrf_radio.h>
#include <zephyr/usb/usb_device.h>
#include <zephyr/drivers/uart.h>

// 根據 Halt 結果，目標絕對在這裡
#define TARGET_FREQ 37 

// 雜訊過濾：只有強於 -70dBm 的才算
// 追蹤器貼近時通常是 -30 到 -50
#define RSSI_THRESHOLD 70 

void radio_config(uint8_t prefix) {
    NRF_RADIO->TASKS_DISABLE = 1;
    while (NRF_RADIO->EVENTS_DISABLED == 0);
    NRF_RADIO->EVENTS_DISABLED = 0;

    nrf_radio_mode_set(NRF_RADIO, NRF_RADIO_MODE_NRF_2MBIT);
    nrf_radio_frequency_set(NRF_RADIO, TARGET_FREQ);

    // 1-Byte Address (BALEN=0)
    nrf_radio_prefix0_set(NRF_RADIO, prefix); 
    nrf_radio_rxaddresses_set(NRF_RADIO, 1); 

    // PCNF0: S1LEN=4 (Based on Halt dump)
    NRF_RADIO->PCNF0 = (8 << RADIO_PCNF0_LFLEN_Pos) | (4 << RADIO_PCNF0_S1LEN_Pos);

    // PCNF1: BALEN=0, Little Endian
    NRF_RADIO->PCNF1 = (32 << RADIO_PCNF1_MAXLEN_Pos) | 
                       (0 << RADIO_PCNF1_BALEN_Pos) | 
                       (RADIO_PCNF1_ENDIAN_Little << RADIO_PCNF1_ENDIAN_Pos) |
                       (RADIO_PCNF1_WHITEEN_Disabled << RADIO_PCNF1_WHITEEN_Pos);

    // CRC
    NRF_RADIO->CRCCNF = (RADIO_CRCCNF_LEN_Two << RADIO_CRCCNF_LEN_Pos) |
                        (RADIO_CRCCNF_SKIPADDR_Include << RADIO_CRCCNF_SKIPADDR_Pos);
    NRF_RADIO->CRCPOLY = 0x11021; 
    NRF_RADIO->CRCINIT = 0xFFFF;

    NRF_RADIO->SHORTS = RADIO_SHORTS_READY_START_Msk | RADIO_SHORTS_END_START_Msk;
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

    printk("\n=== Pico Sniper (CH 37 Only) ===\n");
    printk("Frequency: 2437 MHz. Filter: > -%d dBm\n", RSSI_THRESHOLD);
    printk("Scanning Prefixes... Please keep Tracker flashing!\n");

    uint16_t current_prefix = 0;

    while (1) {
        radio_config((uint8_t)current_prefix);

        // 延長監聽時間：每個地址聽 100ms
        // 這樣掃完一輪 00-FF 需要約 25 秒
        // 優點：絕對不會錯過閃爍
        // 缺點：您需要讓它閃久一點
        for (int i = 0; i < 10; i++) {
            
            if (NRF_RADIO->EVENTS_ADDRESS) {
                NRF_RADIO->EVENTS_ADDRESS = 0;
                
                // 測量強度
                NRF_RADIO->TASKS_RSSISTART = 1;
                while(!NRF_RADIO->EVENTS_RSSIEND);
                NRF_RADIO->EVENTS_RSSIEND = 0;
                
                uint8_t rssi = NRF_RADIO->RSSISAMPLE;

                // 強訊號過濾 (數值越小越強，例如 30 比 80 強)
                if (rssi < RSSI_THRESHOLD) {
                    printk("\n>>> HIT! Prefix: 0x%02X (RSSI: -%d) <<<\n", (uint8_t)current_prefix, rssi);
                    
                    // 如果 CRC 也過了，那就是 100% 中獎
                    if (NRF_RADIO->EVENTS_CRCOK) {
                        printk("!!! CRC OK !!! This is definitely it.\n");
                        // 鎖定
                        while(1) k_sleep(K_FOREVER);
                    }
                }
            }
            k_busy_wait(10000); 
        }

        current_prefix++;
        if (current_prefix > 0xFF) {
            current_prefix = 0;
            printk("."); // 每掃完一輪印一個點，讓您知道它還活著
        }
    }
}

/*
 * Pico Proximity Cracker (RSSI Filtered)
 * Target: Scan 1-byte address on Ch 37.
 * Filter: ONLY accept signals stronger than -50dBm (Very Close).
 * This eliminates Wi-Fi noise.
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <hal/nrf_radio.h>
#include <zephyr/usb/usb_device.h>
#include <zephyr/drivers/uart.h>

#define TARGET_FREQ 37 
// RSSI 門檻：數值越小訊號越強。
// 50 代表 -50dBm。如果數值 < 50 (例如 30)，代表非常近。
// Wi-Fi 雜訊通常在 70-90 之間。
#define RSSI_THRESHOLD 50 

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

    // PCNF1: BALEN=0
    NRF_RADIO->PCNF1 = (32 << RADIO_PCNF1_MAXLEN_Pos) | 
                       (0 << RADIO_PCNF1_BALEN_Pos) | 
                       (RADIO_PCNF1_ENDIAN_Little << RADIO_PCNF1_ENDIAN_Pos) |
                       (RADIO_PCNF1_WHITEEN_Disabled << RADIO_PCNF1_WHITEEN_Pos);

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

    printk("\n=== Pico Proximity Cracker ===\n");
    printk("Scanning Prefixes 00-FF on CH 37.\n");
    printk("NOTE: Place Tracker VERY CLOSE to Dongle!\n");
    printk("Ignoring weak signals (Noise > -%d dBm)...\n", RSSI_THRESHOLD);

    uint16_t current_prefix = 0;

    while (1) {
        radio_config((uint8_t)current_prefix);

        // 每個 Prefix 聽 25ms (快速掃描)
        for (int i = 0; i < 3; i++) {
            
            // 1. 等待地址匹配 (這會因為雜訊一直觸發)
            if (NRF_RADIO->EVENTS_ADDRESS) {
                NRF_RADIO->EVENTS_ADDRESS = 0;
                
                // 2. [關鍵] 馬上測量訊號強度！
                NRF_RADIO->TASKS_RSSISTART = 1;
                
                // 等待測量完成 (硬體只需幾微秒)
                while(!NRF_RADIO->EVENTS_RSSIEND);
                NRF_RADIO->EVENTS_RSSIEND = 0;
                
                uint8_t rssi = NRF_RADIO->RSSISAMPLE;

                // 3. 過濾雜訊
                // 只有當訊號超強 (RSSI < 50) 時，我們才認為這是追蹤器
                if (rssi < RSSI_THRESHOLD) {
                    printk("\n>>> STRONG SIGNAL MATCH! <<<\n");
                    printk("Prefix: 0x%02X (RSSI: -%d dBm)\n", (uint8_t)current_prefix, rssi);
                    
                    // 再次確認：如果是雜訊，CRC 通常會錯。如果是追蹤器，CRC 可能會對。
                    // 這裡我們不鎖定，讓它繼續掃描，看哪個 Prefix 出現最多次
                    k_busy_wait(50000); 
                }
            }
            k_busy_wait(8000); 
        }

        current_prefix++;
        if (current_prefix > 0xFF) {
            current_prefix = 0;
            // 掃完一輪不印點，保持介面乾淨
        }
    }
}

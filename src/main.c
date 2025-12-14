/*
 * Pico Tri-Channel Proximity Scanner
 * Strategy: Scan Ch 1, 37, 77 for 1-byte addresses.
 * Filter: Accept signals stronger than -75dBm (Close range).
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <hal/nrf_radio.h>
#include <zephyr/usb/usb_device.h>
#include <zephyr/drivers/uart.h>

// 掃描這三個 "配對常用頻道"
static uint8_t target_channels[] = {1, 37, 77};
static int ch_index = 0;

// 放寬 RSSI 門檻：-75dBm
// 數值 < 75 才會顯示 (例如 40, 50, 60...)
#define RSSI_THRESHOLD 75 

void radio_config(uint8_t channel, uint8_t prefix) {
    NRF_RADIO->TASKS_DISABLE = 1;
    while (NRF_RADIO->EVENTS_DISABLED == 0);
    NRF_RADIO->EVENTS_DISABLED = 0;

    nrf_radio_mode_set(NRF_RADIO, NRF_RADIO_MODE_NRF_2MBIT);
    nrf_radio_frequency_set(NRF_RADIO, channel);

    // 1-Byte Address (BALEN=0)
    nrf_radio_prefix0_set(NRF_RADIO, prefix); 
    nrf_radio_rxaddresses_set(NRF_RADIO, 1); 

    // PCNF0: S1LEN=4 (標準)
    NRF_RADIO->PCNF0 = (8 << RADIO_PCNF0_LFLEN_Pos) | (4 << RADIO_PCNF0_S1LEN_Pos);

    // PCNF1: BALEN=0 (1-Byte Mode)
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

    printk("\n=== Pico Tri-Channel Scanner ===\n");
    printk("Scanning CH 1, 37, 77. Filter: > -%d dBm\n", RSSI_THRESHOLD);

    uint16_t current_prefix = 0;

    while (1) {
        // 切換頻道
        uint8_t ch = target_channels[ch_index];
        radio_config(ch, (uint8_t)current_prefix);

        // 每個頻道的每個地址聽 15ms (快速掃描)
        for (int i = 0; i < 2; i++) {
            
            if (NRF_RADIO->EVENTS_ADDRESS) {
                NRF_RADIO->EVENTS_ADDRESS = 0;
                
                // 立即測量訊號強度
                NRF_RADIO->TASKS_RSSISTART = 1;
                while(!NRF_RADIO->EVENTS_RSSIEND);
                NRF_RADIO->EVENTS_RSSIEND = 0;
                
                uint8_t rssi = NRF_RADIO->RSSISAMPLE;

                // 只有訊號夠強才印出來
                if (rssi < RSSI_THRESHOLD) {
                    printk("\n>>> HIT! CH %d | Prefix: 0x%02X (RSSI: -%d) <<<\n", ch, (uint8_t)current_prefix, rssi);
                    // 稍微暫停讓我們看清楚
                    k_busy_wait(50000); 
                }
            }
            k_busy_wait(8000); 
        }

        // 邏輯：先掃完一個地址的所有頻道，再換下一個地址
        // 這樣比較容易捕捉到那個瞬間
        ch_index++;
        if (ch_index >= 3) {
            ch_index = 0;
            current_prefix++; // 換下一個地址
            if (current_prefix > 0xFF) current_prefix = 0;
        }
    }
}

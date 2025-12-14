/*
 * Pico Prefix Brute Force (Base 0x43434343)
 * Strategy: Keep Base fixed, rotate Prefix 0x00-0xFF
 * Channel: 77 (Fixed)
 * Endian: Little (Based on dump)
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <hal/nrf_radio.h>
#include <zephyr/usb/usb_device.h>
#include <zephyr/drivers/uart.h>

#define ADDR_BASE_TARGET  0x43434343 // 我們最信任的基底

void radio_config(uint8_t prefix) {
    NRF_RADIO->TASKS_DISABLE = 1;
    while (NRF_RADIO->EVENTS_DISABLED == 0);
    NRF_RADIO->EVENTS_DISABLED = 0;

    nrf_radio_mode_set(NRF_RADIO, NRF_RADIO_MODE_NRF_2MBIT);
    nrf_radio_frequency_set(NRF_RADIO, 77); // 死守 Channel 77

    // 設定地址：Base 固定，Prefix 變動
    nrf_radio_base0_set(NRF_RADIO, ADDR_BASE_TARGET); 
    nrf_radio_prefix0_set(NRF_RADIO, prefix); // 測試這個 Prefix
    nrf_radio_rxaddresses_set(NRF_RADIO, 1);  // Enable Logical Address 0

    // PCNF0: Standard
    NRF_RADIO->PCNF0 = (8 << RADIO_PCNF0_LFLEN_Pos) | (4 << RADIO_PCNF0_S1LEN_Pos);

    // PCNF1: Standard BALEN=4, Little Endian, No Whitening
    NRF_RADIO->PCNF1 = (32 << RADIO_PCNF1_MAXLEN_Pos) | 
                       (4 << RADIO_PCNF1_BALEN_Pos) | // 標準 4-byte Base
                       (RADIO_PCNF1_ENDIAN_Little << RADIO_PCNF1_ENDIAN_Pos) |
                       (RADIO_PCNF1_WHITEEN_Disabled << RADIO_PCNF1_WHITEEN_Pos);

    // CRC (Doesn't matter for address match, but set it anyway)
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

    printk("\n=== Pico Prefix Scanner (Base 0x43434343) ===\n");
    printk("Scanning Prefix 0x00 - 0xFF...\n");

    uint16_t current_prefix = 0;

    while (1) {
        radio_config((uint8_t)current_prefix);

        // 每個 Prefix 聽 50ms
        // 追蹤器廣播間隔通常 < 20ms，所以 50ms 足夠抓到它
        for (int i = 0; i < 5; i++) {
            
            // [!] 物理地址匹配 [!]
            if (NRF_RADIO->EVENTS_ADDRESS) {
                NRF_RADIO->EVENTS_ADDRESS = 0;
                
                printk("\n>>> MATCH FOUND! <<<\n");
                printk("Correct Prefix: 0x%02X\n", (uint8_t)current_prefix);
                printk("Full Address: 0x%02X + 0x%08X\n", (uint8_t)current_prefix, ADDR_BASE_TARGET);
                
                // 鎖定成功，不再掃描
                while(1) {
                    k_busy_wait(100000);
                    if (NRF_RADIO->EVENTS_ADDRESS) {
                        NRF_RADIO->EVENTS_ADDRESS = 0;
                        printk("!"); // 持續閃爍驚嘆號代表訊號穩定
                    }
                }
            }
            k_busy_wait(10000); 
        }

        current_prefix++;
        if (current_prefix > 0xFF) {
            current_prefix = 0;
            // printk("."); // 掃完一輪印個點，證明還活著
        }
    }
    return 0;
}

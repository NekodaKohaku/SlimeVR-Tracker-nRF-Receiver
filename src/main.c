/*
 * Pico ULTIMATE Scanner
 * Strategy: Scan all 256 possible 1-byte addresses on Channel 77.
 * Config: Little Endian, No Whitening, BALEN=0.
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <hal/nrf_radio.h>
#include <zephyr/usb/usb_device.h>
#include <zephyr/drivers/uart.h>

// 目標頻率 2477 MHz
#define CAMP_FREQ 77

void radio_config(uint8_t prefix_byte) {
    NRF_RADIO->TASKS_DISABLE = 1;
    while (NRF_RADIO->EVENTS_DISABLED == 0);
    NRF_RADIO->EVENTS_DISABLED = 0;

    nrf_radio_mode_set(NRF_RADIO, NRF_RADIO_MODE_NRF_2MBIT);
    nrf_radio_frequency_set(NRF_RADIO, CAMP_FREQ);

    // 關鍵設定：BALEN=0 (不使用 Base Address)
    // 我們直接把要猜的那個 Byte 設為 Prefix0
    nrf_radio_prefix0_set(NRF_RADIO, prefix_byte); 
    nrf_radio_rxaddresses_set(NRF_RADIO, 1); // Enable Logical Address 0

    // PCNF0: 根據 Dump (S1LEN=4 is critical!)
    NRF_RADIO->PCNF0 = (8 << RADIO_PCNF0_LFLEN_Pos) | (4 << RADIO_PCNF0_S1LEN_Pos);

    // PCNF1: Little Endian, No White, BALEN=0
    NRF_RADIO->PCNF1 = (32 << RADIO_PCNF1_MAXLEN_Pos) | 
                       (0 << RADIO_PCNF1_BALEN_Pos) | // BALEN=0
                       (RADIO_PCNF1_ENDIAN_Little << RADIO_PCNF1_ENDIAN_Pos) |
                       (RADIO_PCNF1_WHITEEN_Disabled << RADIO_PCNF1_WHITEEN_Pos);

    // CRC: Standard 2 Bytes
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

    printk("\n=== Pico 1-Byte Scanner ===\n");
    printk("Scanning Address 0x00 - 0xFF on CH 77...\n");

    uint8_t current_byte = 0;

    while (1) {
        // 設定目前的猜測地址
        radio_config(current_byte);

        // 每個地址聽 15ms (Pico 廣播間隔通常很短，這樣夠了)
        // 快速掃描一輪 256 個地址只需要約 3.8 秒
        for (int i = 0; i < 2; i++) {
            if (NRF_RADIO->EVENTS_END) {
                NRF_RADIO->EVENTS_END = 0;
                
                // 如果 CRC OK，代表我們猜對了那個 Byte！
                if (NRF_RADIO->EVENTS_CRCOK) {
                    printk("\n>>> JACKPOT!!! <<<\n");
                    printk("Found Correct Address Byte: 0x%02X\n", current_byte);
                    printk("Data Received on CH 77!\n");
                    
                    // 鎖定這個地址，不再掃描
                    while(1) {
                        k_busy_wait(100000);
                        if (NRF_RADIO->EVENTS_CRCOK) {
                             NRF_RADIO->EVENTS_CRCOK = 0;
                             printk(".");
                        }
                    }
                }
            }
            k_busy_wait(7500); // 7.5ms per loop
        }

        current_byte++; // 下一個地址
        // 讓它自動溢位 0xFF -> 0x00，無限循環
    }
    return 0;
}

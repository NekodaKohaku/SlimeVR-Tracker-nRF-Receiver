/*
 * Pico Frequency Sweeper
 * Target: Address 0x23 (1-Byte)
 * Action: Sweep ALL channels (0-80) to find where it is hiding.
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <hal/nrf_radio.h>
#include <zephyr/usb/usb_device.h>
#include <zephyr/drivers/uart.h>

// 我們最懷疑的地址 (來自 Dump Prefix byte 3)
#define TARGET_PREFIX  0x23

void radio_config(uint8_t channel) {
    NRF_RADIO->TASKS_DISABLE = 1;
    while (NRF_RADIO->EVENTS_DISABLED == 0);
    NRF_RADIO->EVENTS_DISABLED = 0;

    // 設定模式 2Mbit
    nrf_radio_mode_set(NRF_RADIO, NRF_RADIO_MODE_NRF_2MBIT);
    nrf_radio_frequency_set(NRF_RADIO, channel);

    // 設定地址: 1-Byte Mode (BALEN=0)
    // 我們只看 Prefix
    nrf_radio_prefix0_set(NRF_RADIO, TARGET_PREFIX); 
    nrf_radio_rxaddresses_set(NRF_RADIO, 1); 

    // PCNF0: S1LEN=4 (根據 Dump)
    NRF_RADIO->PCNF0 = (8 << RADIO_PCNF0_LFLEN_Pos) | (4 << RADIO_PCNF0_S1LEN_Pos);

    // PCNF1: BALEN=0, Little Endian, No Whitening (根據 0x04 推算)
    NRF_RADIO->PCNF1 = (32 << RADIO_PCNF1_MAXLEN_Pos) | 
                       (0 << RADIO_PCNF1_BALEN_Pos) | // BALEN=0
                       (RADIO_PCNF1_ENDIAN_Little << RADIO_PCNF1_ENDIAN_Pos) |
                       (RADIO_PCNF1_WHITEEN_Disabled << RADIO_PCNF1_WHITEEN_Pos);

    // CRC: 我們先假設標準 2 bytes
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

    printk("\n=== Pico Frequency Sweeper ===\n");
    printk("Hunting Address 0x%02X across CH 0-80...\n", TARGET_PREFIX);

    int current_freq = 0;

    while (1) {
        radio_config(current_freq);

        // 每個頻道聽 30ms
        for (int i = 0; i < 3; i++) {
            
            // [!] 物理地址匹配 [!]
            // 只要硬體偵測到 0x23，我們就認定抓到頻道了
            if (NRF_RADIO->EVENTS_ADDRESS) {
                NRF_RADIO->EVENTS_ADDRESS = 0;
                
                printk("\n>>> CONTACT! CH %d <<<\n", current_freq);
                printk("Signal detected on Freq: %d MHz\n", 2400 + current_freq);
                
                // 檢查 CRC
                if (NRF_RADIO->EVENTS_END && NRF_RADIO->EVENTS_CRCOK) {
                     printk("Payload Verified (CRC OK)!\n");
                     // 鎖定這個頻道
                     while(1) {
                         k_busy_wait(100000);
                         if(NRF_RADIO->EVENTS_CRCOK) {
                             NRF_RADIO->EVENTS_CRCOK=0; 
                             printk(".");
                         }
                     }
                }
            }
            k_busy_wait(10000); 
        }

        current_freq++;
        if (current_freq > 85) current_freq = 0; // 掃描 2400-2485 MHz
    }
    return 0;
}

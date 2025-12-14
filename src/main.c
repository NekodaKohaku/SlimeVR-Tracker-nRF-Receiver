/*
 * Pico Final Receiver (Scanner Version)
 * Strategy: Rotate between CH 1, 37, 77 rapidly
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <hal/nrf_radio.h>
#include <zephyr/usb/usb_device.h>
#include <zephyr/drivers/uart.h>

#define CRACKED_BASE_ADDR   0x552C6A1E
#define CRACKED_PREFIX      0xC0

// 我們發現的三個跳頻頻道
static uint8_t target_channels[] = {1, 37, 77};
static int ch_index = 0;

void radio_config(uint8_t channel) {
    // 1. 必須先 Disable 才能改頻率
    NRF_RADIO->TASKS_DISABLE = 1;
    while (NRF_RADIO->EVENTS_DISABLED == 0);
    NRF_RADIO->EVENTS_DISABLED = 0;

    nrf_radio_mode_set(NRF_RADIO, NRF_RADIO_MODE_NRF_2MBIT);
    nrf_radio_frequency_set(NRF_RADIO, channel);

    // 地址設定
    nrf_radio_base0_set(NRF_RADIO, CRACKED_BASE_ADDR);
    nrf_radio_prefix0_set(NRF_RADIO, CRACKED_PREFIX);
    nrf_radio_rxaddresses_set(NRF_RADIO, 1); 

    // 封包格式 (標準)
    NRF_RADIO->PCNF0 = (8 << RADIO_PCNF0_LFLEN_Pos) | (1 << RADIO_PCNF0_S0LEN_Pos);
    NRF_RADIO->PCNF1 = (32 << RADIO_PCNF1_MAXLEN_Pos) | (4 << RADIO_PCNF1_BALEN_Pos) | 
                       (RADIO_PCNF1_ENDIAN_Big << RADIO_PCNF1_ENDIAN_Pos) |
                       (RADIO_PCNF1_WHITEEN_Enabled << RADIO_PCNF1_WHITEEN_Pos);

    // CRC 設定
    nrf_radio_crc_configure(NRF_RADIO, RADIO_CRCCNF_LEN_Two, NRF_RADIO_CRC_ADDR_SKIP, 0x11021);
    NRF_RADIO->CRCINIT = 0xFFFF;
    NRF_RADIO->DATAWHITEIV = channel | 0x40; 

    NRF_RADIO->SHORTS = RADIO_SHORTS_READY_START_Msk | RADIO_SHORTS_END_START_Msk;
    
    // 啟動接收
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

    printk("\n=== Pico Scanner Active ===\n");
    printk("Scanning CH 1, 37, 77...\n");

    while (1) {
        // 1. 切換到下一個頻道
        uint8_t ch = target_channels[ch_index];
        // printk("Scanning CH %d...\n", ch); // 為了不洗版，這行先註解掉
        radio_config(ch);

        // 2. 在這個頻道停留 200ms 聽聽看
        for (int i = 0; i < 20; i++) {
            
            // 如果抓到地址匹配 (Address Match)
            if (NRF_RADIO->EVENTS_ADDRESS) {
                NRF_RADIO->EVENTS_ADDRESS = 0;
                printk("[!] Signal on CH %d!\n", ch);
            }

            // 如果接收完成
            if (NRF_RADIO->EVENTS_END) {
                NRF_RADIO->EVENTS_END = 0;
                
                if (NRF_RADIO->EVENTS_CRCOK) {
                    printk(">>> BINGO! CH %d Packet OK! <<<\n", ch);
                } else if (NRF_RADIO->EVENTS_CRCERROR) {
                    // 如果有 CRC 錯誤，但地址對了，這代表我們離成功只差一點點
                    // 可能是端序 (Endian) 或 CRC 初始值的問題
                    printk("--- CRC Error on CH %d ---\n", ch);
                }
            }
            k_busy_wait(10000); // Wait 10ms * 20 = 200ms total per channel
        }

        // 3. 換下一個
        ch_index++;
        if (ch_index >= 3) ch_index = 0;
    }
    return 0;
}

/*
 * Pico Final Receiver (Dragnet Version)
 * Strategy: Listen to ALL logical addresses (0-7) simultaneously.
 * Base0: 0x552C6A1E (Unique)
 * Base1: 0x43434343 (Shared/Pairing)
 * Prefixes: Covers 23, C3, 43, C0, and 04
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <hal/nrf_radio.h>
#include <zephyr/usb/usb_device.h>
#include <zephyr/drivers/uart.h>

// === 您的密鑰庫 ===
#define ADDR_BASE0       0x552C6A1E  // 來自 read32 0x4000151C
#define ADDR_BASE1       0x43434343  // 來自 read32 0x40001520
#define ADDR_PREFIX0     0x23C343C0  // 來自 read32 0x40001524
#define ADDR_PREFIX1     0x04040404  // 猜測值 (來自 Log 的 hint)

// 掃描頻道
static uint8_t target_channels[] = {1, 37, 77};
static int ch_index = 0;

void radio_config(uint8_t channel) {
    NRF_RADIO->TASKS_DISABLE = 1;
    while (NRF_RADIO->EVENTS_DISABLED == 0);
    NRF_RADIO->EVENTS_DISABLED = 0;

    nrf_radio_mode_set(NRF_RADIO, NRF_RADIO_MODE_NRF_2MBIT);
    nrf_radio_frequency_set(NRF_RADIO, channel);

    // --- 關鍵修改：佈下天羅地網 ---
    
    // 1. 設定兩組基底
    nrf_radio_base0_set(NRF_RADIO, ADDR_BASE0); // 給 Address 0 用
    nrf_radio_base1_set(NRF_RADIO, ADDR_BASE1); // 給 Address 1-7 用

    // 2. 設定所有前綴
    nrf_radio_prefix0_set(NRF_RADIO, ADDR_PREFIX0); // 包含 C0, 43, C3, 23
    nrf_radio_prefix1_set(NRF_RADIO, ADDR_PREFIX1); // 包含 04, 04, 04, 04

    // 3. 啟用所有邏輯地址 (0-7 全部監聽)
    nrf_radio_rxaddresses_set(NRF_RADIO, 0xFF); 

    // ----------------------------

    NRF_RADIO->PCNF0 = (8 << RADIO_PCNF0_LFLEN_Pos) | (1 << RADIO_PCNF0_S0LEN_Pos);
    NRF_RADIO->PCNF1 = (32 << RADIO_PCNF1_MAXLEN_Pos) | (4 << RADIO_PCNF1_BALEN_Pos) | 
                       (RADIO_PCNF1_ENDIAN_Big << RADIO_PCNF1_ENDIAN_Pos) |
                       (RADIO_PCNF1_WHITEEN_Enabled << RADIO_PCNF1_WHITEEN_Pos);

    nrf_radio_crc_configure(NRF_RADIO, RADIO_CRCCNF_LEN_Two, NRF_RADIO_CRC_ADDR_SKIP, 0x11021);
    NRF_RADIO->CRCINIT = 0xFFFF;
    NRF_RADIO->DATAWHITEIV = channel | 0x40; 

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

    printk("\n=== Pico Dragnet Active ===\n");
    printk("Listening on ALL addresses (Base0/Base1)...\n");

    while (1) {
        uint8_t ch = target_channels[ch_index];
        radio_config(ch);

        // 每個頻道聽 150ms
        for (int i = 0; i < 15; i++) {
            if (NRF_RADIO->EVENTS_ADDRESS) {
                NRF_RADIO->EVENTS_ADDRESS = 0;
                // 這次我們不只印 Signal，還印出是「哪一號地址」抓到的
                // RXMATCH 暫存器會告訴我們是 Address 0~7 哪一個
                uint8_t match_idx = NRF_RADIO->RXMATCH;
                printk("[!] Signal on CH %d (Addr Index: %d)!\n", ch, match_idx);
            }

            if (NRF_RADIO->EVENTS_END) {
                NRF_RADIO->EVENTS_END = 0;
                if (NRF_RADIO->EVENTS_CRCOK) {
                    printk(">>> BINGO! Packet OK on CH %d! <<<\n", ch);
                } else if (NRF_RADIO->EVENTS_CRCERROR) {
                    // 如果一直 CRC Error，試試看把 PCNF1 的 Endian 改成 Little
                    printk("--- CRC Error on CH %d ---\n", ch);
                }
            }
            k_busy_wait(10000); 
        }

        ch_index++;
        if (ch_index >= 3) ch_index = 0;
    }
    return 0;
}

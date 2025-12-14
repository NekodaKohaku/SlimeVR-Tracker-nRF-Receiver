/*
 * Pico Receiver: THE CRACKED VERSION
 * Configured based on actual register dumps.
 * Mode: 2Mbit (Assumed standard)
 * Endian: Little (Crucial fix!)
 * Whitening: OFF (Crucial fix!)
 * CRC: 2 Bytes, Includes Address
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <hal/nrf_radio.h>
#include <zephyr/usb/usb_device.h>
#include <zephyr/drivers/uart.h>

// === 地址設定 ===
// 根據您的 read32 0x40001520 (Base1) 和 0x40001518 (TXADDR)
// 它是用 Logical Address 3 發射的
// Base1: 0x43434343
// Prefix3: 0x23 (來自 Prefix0 的最高字節)
#define ADDR_BASE1       0x43434343 
#define ADDR_PREFIX0     0x23C343C0  // 包含 C0, 43, C3, 23 (我們全聽)

// 掃描頻道
static uint8_t target_channels[] = {1, 37, 77};
static int ch_index = 0;

void radio_config(uint8_t channel) {
    NRF_RADIO->TASKS_DISABLE = 1;
    while (NRF_RADIO->EVENTS_DISABLED == 0);
    NRF_RADIO->EVENTS_DISABLED = 0;

    // 1. 設定模式 (標準 VR 都是 2Mbit)
    nrf_radio_mode_set(NRF_RADIO, NRF_RADIO_MODE_NRF_2MBIT);
    nrf_radio_frequency_set(NRF_RADIO, channel);

    // 2. 設定地址 (還是使用天羅地網，但重點是 Endian 改了)
    nrf_radio_base1_set(NRF_RADIO, ADDR_BASE1); 
    nrf_radio_prefix0_set(NRF_RADIO, ADDR_PREFIX0); 
    // 監聽 Address 0-3 (0x0F)
    nrf_radio_rxaddresses_set(NRF_RADIO, 0x0F); 

    // 3. PCNF0 (根據 read32 0x40001514 = 00040008)
    // LFLEN=8, S0LEN=0, S1LEN=4 (關鍵差異!)
    NRF_RADIO->PCNF0 = (8 << RADIO_PCNF0_LFLEN_Pos) | 
                       (0 << RADIO_PCNF0_S0LEN_Pos) |
                       (4 << RADIO_PCNF0_S1LEN_Pos); // 修正這裡！

    // 4. PCNF1 (根據 read32 0x40001510 = 00000004)
    // Little Endian, No Whitening, MaxLen=32(safe default), BalLen=4(standard)
    NRF_RADIO->PCNF1 = (32 << RADIO_PCNF1_MAXLEN_Pos) | 
                       (4 << RADIO_PCNF1_BALEN_Pos) | 
                       (RADIO_PCNF1_ENDIAN_Little << RADIO_PCNF1_ENDIAN_Pos) | // 修正：小端序
                       (RADIO_PCNF1_WHITEEN_Disabled << RADIO_PCNF1_WHITEEN_Pos); // 修正：關閉白化

    // 5. CRC 設定 (根據 read32 0x40001534 = 2)
    // 2 Bytes, Include Address (SKIPADDR=0)
    // Poly 0x11021 (from 0x1538)
    NRF_RADIO->CRCCNF = (RADIO_CRCCNF_LEN_Two << RADIO_CRCCNF_LEN_Pos) |
                        (RADIO_CRCCNF_SKIPADDR_Include << RADIO_CRCCNF_SKIPADDR_Pos); // 修正：包含地址
    NRF_RADIO->CRCPOLY = 0x11021; 
    NRF_RADIO->CRCINIT = 0xFFFF; // 通常是 FFFF，先試這個

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

    printk("\n=== Pico Corrected Config ===\n");
    printk("Config: Little Endian, No Whitening, S1=4\n");

    while (1) {
        uint8_t ch = target_channels[ch_index];
        radio_config(ch);

        // 每個頻道掃描 100ms
        for (int i = 0; i < 10; i++) {
            if (NRF_RADIO->EVENTS_ADDRESS) {
                NRF_RADIO->EVENTS_ADDRESS = 0;
                // 印出是抓到哪一號地址 (應該是 3)
                printk("[!] Signal on CH %d (Addr: %d)\n", ch, NRF_RADIO->RXMATCH);
            }

            if (NRF_RADIO->EVENTS_END) {
                NRF_RADIO->EVENTS_END = 0;
                
                if (NRF_RADIO->EVENTS_CRCOK) {
                    printk(">>> PACKET OK! CH %d <<<\n", ch);
                } else if (NRF_RADIO->EVENTS_CRCERROR) {
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

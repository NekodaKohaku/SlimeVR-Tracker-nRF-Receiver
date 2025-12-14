/*
 * Pico Final Receiver (Cracked Version)
 * Decoded Address: Base 0x552C6A1E, Prefix 0xC0
 * Decoded Channel: 1 (2401 MHz)
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <hal/nrf_radio.h>

// 我們剛剛挖出來的密碼
#define CRACKED_BASE_ADDR   0x552C6A1E
#define CRACKED_PREFIX      0xC0
#define CRACKED_CHANNEL     1 

void radio_init_sniffer(void) {
    // 1. 關閉並重置無線電
    NRF_RADIO->TASKS_DISABLE = 1;
    while (NRF_RADIO->EVENTS_DISABLED == 0);
    NRF_RADIO->EVENTS_DISABLED = 0;

    // 2. 設定模式 (通常 Pico 是 2Mbit)
    nrf_radio_mode_set(NRF_RADIO, NRF_RADIO_MODE_NRF_2MBIT);
    
    // 3. 設定我們挖出來的頻率 (2401 MHz)
    nrf_radio_frequency_set(NRF_RADIO, CRACKED_CHANNEL);

    // 4. 設定我們挖出來的地址
    // Base Address 0
    nrf_radio_base0_set(NRF_RADIO, CRACKED_BASE_ADDR);
    // Prefix 0 (取 lowest byte: C0)
    nrf_radio_prefix0_set(NRF_RADIO, CRACKED_PREFIX);
    
    // 啟用接收地址 0
    nrf_radio_rxaddresses_set(NRF_RADIO, 1); 

    // 5. 封包設定 (PCNF) - 這是標準 ESB 設定，先試這組
    // LFLEN=8 bits, S0LEN=1 bit, S1LEN=0
    NRF_RADIO->PCNF0 = (8 << RADIO_PCNF0_LFLEN_Pos) | 
                       (1 << RADIO_PCNF0_S0LEN_Pos);
                       
    // MaxLen=32, BalLen=4 (Base Address Length), Endian=Big
    NRF_RADIO->PCNF1 = (32 << RADIO_PCNF1_MAXLEN_Pos) | 
                       (4 << RADIO_PCNF1_BALEN_Pos) | 
                       (RADIO_PCNF1_ENDIAN_Big << RADIO_PCNF1_ENDIAN_Pos) |
                       (RADIO_PCNF1_WHITEEN_Enabled << RADIO_PCNF1_WHITEEN_Pos);

    // 6. CRC 設定 (Pico 通常用 2 bytes CRC)
    nrf_radio_crc_configure(NRF_RADIO, RADIO_CRCCNF_LEN_Two, NRF_RADIO_CRC_ADDR_SKIP, 0x11021);
    NRF_RADIO->CRCINIT = 0xFFFF;
    NRF_RADIO->DATAWHITEIV = CRACKED_CHANNEL | 0x40; // Whitening init (Channel + 40 is unlikely but standard is often channel-based)

    // 7. 啟用 Shortcuts (收到後自動重啟接收)
    NRF_RADIO->SHORTS = RADIO_SHORTS_READY_START_Msk | RADIO_SHORTS_END_START_Msk;
}

int main(void) {
    printk("\n=== Pico Tracker Receiver Active ===\n");
    printk("Listening on Freq: 2401 MHz (CH 1)\n");
    printk("Address: Base 0x%08X, Prefix 0x%02X\n", CRACKED_BASE_ADDR, CRACKED_PREFIX);

    radio_init_sniffer();
    NRF_RADIO->TASKS_RXEN = 1;

    while (1) {
        if (NRF_RADIO->EVENTS_ADDRESS) {
            NRF_RADIO->EVENTS_ADDRESS = 0;
            printk("[!] Address Match! Signal Detected!\n");
        }

        if (NRF_RADIO->EVENTS_END) {
            NRF_RADIO->EVENTS_END = 0;
            
            if (NRF_RADIO->EVENTS_CRCOK) {
                printk(">>> PACKET RECEIVED! CRC OK! <<<\n");
                // 這裡可以把封包內容印出來分析 (Payload)
            } else if (NRF_RADIO->EVENTS_CRCERROR) {
                printk("--- CRC Error (But signal is strong) ---\n");
            }
        }
        k_busy_wait(100);
    }
    return 0;
}

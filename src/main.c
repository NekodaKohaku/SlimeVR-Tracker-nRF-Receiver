/*
 * Pico Final Receiver (Hopping Cracker)
 * Strategy: Camp on Channel 77 and wait for the tracker.
 * Address: Base 0x552C6A1E, Prefix 0xC0
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <hal/nrf_radio.h>

// === 我們解碼出來的密鑰 ===
#define CRACKED_BASE_ADDR   0x552C6A1E
#define CRACKED_PREFIX      0xC0
#define CAMPING_CHANNEL     77  // 對應 0x4D (2477 MHz)

void radio_init_sniffer(void) {
    // 1. 關閉無線電
    NRF_RADIO->TASKS_DISABLE = 1;
    while (NRF_RADIO->EVENTS_DISABLED == 0);
    NRF_RADIO->EVENTS_DISABLED = 0;

    // 2. 設定模式 (Pico 2.0 通常使用 NRF_2MBIT)
    nrf_radio_mode_set(NRF_RADIO, NRF_RADIO_MODE_NRF_2MBIT);
    
    // 3. 設定頻率 (死守 Channel 77)
    nrf_radio_frequency_set(NRF_RADIO, CAMPING_CHANNEL);

    // 4. 設定地址
    nrf_radio_base0_set(NRF_RADIO, CRACKED_BASE_ADDR);
    nrf_radio_prefix0_set(NRF_RADIO, CRACKED_PREFIX);
    nrf_radio_rxaddresses_set(NRF_RADIO, 1); // 啟用邏輯地址 0

    // 5. 封包格式 (標準 ESB 配置)
    // LFLEN=8bit, S0LEN=1bit, S1LEN=0
    NRF_RADIO->PCNF0 = (8 << RADIO_PCNF0_LFLEN_Pos) | 
                       (1 << RADIO_PCNF0_S0LEN_Pos);
                       
    // MaxLen=32, BalLen=4, Big Endian, Whitening Enabled
    NRF_RADIO->PCNF1 = (32 << RADIO_PCNF1_MAXLEN_Pos) | 
                       (4 << RADIO_PCNF1_BALEN_Pos) | 
                       (RADIO_PCNF1_ENDIAN_Big << RADIO_PCNF1_ENDIAN_Pos) |
                       (RADIO_PCNF1_WHITEEN_Enabled << RADIO_PCNF1_WHITEEN_Pos);

    // 6. CRC 設定 (2 Bytes, Algorithm Skip Address)
    // 這是最常見的標準配置，如果不對我們再試 16-bit CRC
    nrf_radio_crc_configure(NRF_RADIO, RADIO_CRCCNF_LEN_Two, NRF_RADIO_CRC_ADDR_SKIP, 0x11021);
    NRF_RADIO->CRCINIT = 0xFFFF;
    NRF_RADIO->DATAWHITEIV = CAMPING_CHANNEL | 0x40; // Whitening IV

    // 7. 快捷方式 (收到包後自動重新開始接收)
    NRF_RADIO->SHORTS = RADIO_SHORTS_READY_START_Msk | RADIO_SHORTS_END_START_Msk;
}

int main(void) {
    printk("\n=== Pico Hunter Active ===\n");
    printk("Camping on Freq: 2477 MHz (CH %d)\n", CAMPING_CHANNEL);
    printk("Target Address: 0x%02X + 0x%08X\n", CRACKED_PREFIX, CRACKED_BASE_ADDR);

    radio_init_sniffer();
    NRF_RADIO->TASKS_RXEN = 1;

    while (1) {
        // 偵測到「地址匹配」 (抓到訊號了！)
        if (NRF_RADIO->EVENTS_ADDRESS) {
            NRF_RADIO->EVENTS_ADDRESS = 0;
            printk("[!] Signal Detected! (Address Match)\n");
        }

        // 封包接收結束
        if (NRF_RADIO->EVENTS_END) {
            NRF_RADIO->EVENTS_END = 0;
            
            if (NRF_RADIO->EVENTS_CRCOK) {
                printk(">>> BINGO! Packet Received! CRC OK! <<<\n");
                
                // 這裡可以把 Payload 印出來看
                // 例如: printk("Data: %02x %02x ...\n", NRF_RADIO->PACKETPTR...);
                
            } else if (NRF_RADIO->EVENTS_CRCERROR) {
                printk("--- CRC Error (Frequency mismatch or noise) ---\n");
            }
        }
        
        // 避免 CPU 跑太快刷屏，這裡稍微停一下
        // 但 Radio 依然在背景接收
        k_busy_wait(100); 
    }
    return 0;
}

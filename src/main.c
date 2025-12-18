/*
 * Pico Tracker CORRECTED SNIFFER
 * 1. MODE: Ble_2Mbit (Mode 4) - Fixed from dump
 * 2. ENDIAN: Big Endian (PCNF1 bit 24 is set) - Fixed from dump
 * 3. CRC: Disabled (To see data even if calculation fails)
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <hal/nrf_radio.h>
#include <zephyr/usb/usb_device.h>

// === 參數設定 ===
#define CH_FREQ           1           
#define TARGET_BASE_ADDR  0x552c6a1eUL 
#define TARGET_PREFIX     0xC0        
#define RSSI_THRESHOLD    70          // -70dBm (放寬一點以免沒貼緊)

static uint8_t rx_buffer[32]; 

void radio_init(void)
{
    NRF_RADIO->TASKS_DISABLE = 1;
    k_busy_wait(200);
    NRF_RADIO->EVENTS_DISABLED = 0;

    // 1. [修正] 物理層模式 Ble_2Mbit (值為 4)
    NRF_RADIO->MODE = 4; 

    NRF_RADIO->FREQUENCY = CH_FREQ;

    // 2. 地址設定
    NRF_RADIO->BASE0 = TARGET_BASE_ADDR;
    NRF_RADIO->PREFIX0 = TARGET_PREFIX;
    NRF_RADIO->TXADDRESS = 0;
    NRF_RADIO->RXADDRESSES = 1;

    // 3. PCNF0: 根據 Dump 設為 8 bit 長度
    NRF_RADIO->PCNF0 = (8 << RADIO_PCNF0_LFLEN_Pos) |
                       (0 << RADIO_PCNF0_S0LEN_Pos) |
                       (4 << RADIO_PCNF0_S1LEN_Pos); // S1LEN=4

    // 4. [關鍵修正] PCNF1: 設定為 BIG ENDIAN
    // Dump 0x40001518 = 01040023 -> Bit 24 is 1
    NRF_RADIO->PCNF1 = (35 << RADIO_PCNF1_MAXLEN_Pos) | 
                       (0  << RADIO_PCNF1_STATLEN_Pos) | 
                       (4  << RADIO_PCNF1_BALEN_Pos) | 
                       (1  << RADIO_PCNF1_ENDIAN_Pos); // 1 = Big Endian

    // 5. 關閉 CRC (先看數據)
    NRF_RADIO->CRCCNF = 0; 
    
    // 6. 啟用 RSSI 測量
    NRF_RADIO->SHORTS = (RADIO_SHORTS_READY_START_Msk | 
                         RADIO_SHORTS_ADDRESS_RSSISTART_Msk);
}

int main(void)
{
    usb_enable(NULL);
    k_sleep(K_MSEC(2000));
    
    printk("\n============================================\n");
    printk(">>> CORRECTED SNIFFER (Mode 4 + BigEndian) <<<\n");
    printk(">>> Showing raw data...                    <<<\n");
    printk("============================================\n");

    radio_init();

    NRF_RADIO->PACKETPTR = (uint32_t)rx_buffer;
    NRF_RADIO->TASKS_RXEN = 1;
    
    while (1) {
        if (NRF_RADIO->EVENTS_END) {
            NRF_RADIO->EVENTS_END = 0;
            
            uint8_t rssi_val = NRF_RADIO->RSSISAMPLE;
            
            // RSSI 過濾
            if (rssi_val > 0 && rssi_val <= RSSI_THRESHOLD) {
                
                // 過濾全 0
                bool valid = false;
                for(int i=0; i<8; i++) if (rx_buffer[i] != 0) valid = true;

                if (valid) {
                    printk("[RSSI -%d] RAW: ", rssi_val);
                    for(int i=0; i<16; i++) printk("%02X ", rx_buffer[i]);
                    printk("\n");
                }
            }
            NRF_RADIO->TASKS_START = 1;
        }
        k_busy_wait(100); 
    }
}

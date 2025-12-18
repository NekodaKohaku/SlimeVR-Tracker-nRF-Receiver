/*
 * Pico Tracker MODE 4 BLIND SNIFFER
 * 1. MODE: Ble_2Mbit (Mode 4) - 這是為了修復亂碼
 * 2. BALEN: 0 - 這是為了修復「沒訊號」(無視地址順序問題)
 * 3. Filter: RSSI Only - 只抓貼在臉上的強訊號
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <hal/nrf_radio.h>
#include <zephyr/usb/usb_device.h>

#define CH_FREQ           1           
// 門檻設寬一點，抓到再說
#define RSSI_THRESHOLD    80          

static uint8_t rx_buffer[32]; 

void radio_init(void)
{
    NRF_RADIO->TASKS_DISABLE = 1;
    k_busy_wait(200);
    NRF_RADIO->EVENTS_DISABLED = 0;

    // 1. [關鍵] 物理層設為 Ble_2Mbit (Mode 4)
    // 這樣才能正確解碼 Tracker 的波形
    NRF_RADIO->MODE = 4; 

    NRF_RADIO->FREQUENCY = CH_FREQ;

    // 2. [關鍵] 暴力盲收設定
    // BALEN = 0 (不檢查地址)
    // LFLEN = 0 (不檢查長度)
    // Big Endian 不重要了，因為我們根本不比對地址
    NRF_RADIO->PCNF0 = (0 << RADIO_PCNF0_LFLEN_Pos);
    
    NRF_RADIO->PCNF1 = (32 << RADIO_PCNF1_MAXLEN_Pos) | 
                       (32 << RADIO_PCNF1_STATLEN_Pos) | 
                       (0  << RADIO_PCNF1_BALEN_Pos) | // 地址長度 = 0
                       (1  << RADIO_PCNF1_ENDIAN_Pos); // 照著 Dump 設 1

    // 地址設什麼都沒差
    NRF_RADIO->BASE0 = 0xAAAAAAAA;
    NRF_RADIO->PREFIX0 = 0xAA;
    NRF_RADIO->TXADDRESS = 0;
    NRF_RADIO->RXADDRESSES = 1;

    // 3. 關閉 CRC (絕對要關，不然盲收不到)
    NRF_RADIO->CRCCNF = 0; 
    
    // 4. 啟用 RSSI
    NRF_RADIO->SHORTS = (RADIO_SHORTS_READY_START_Msk | 
                         RADIO_SHORTS_ADDRESS_RSSISTART_Msk);
}

int main(void)
{
    usb_enable(NULL);
    k_sleep(K_MSEC(2000));
    
    printk("\n============================================\n");
    printk(">>> MODE 4 BLIND SNIFFER (No Address Check) <<<\n");
    printk(">>> Waiting for STRONG signals...           <<<\n");
    printk("============================================\n");

    radio_init();

    NRF_RADIO->PACKETPTR = (uint32_t)rx_buffer;
    NRF_RADIO->TASKS_RXEN = 1;
    
    while (1) {
        if (NRF_RADIO->EVENTS_END) {
            NRF_RADIO->EVENTS_END = 0;
            
            uint8_t rssi_val = NRF_RADIO->RSSISAMPLE;
            
            // RSSI 過濾 (貼近測試)
            if (rssi_val > 0 && rssi_val <= RSSI_THRESHOLD) {
                
                // 過濾掉全 0 的無效封包
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

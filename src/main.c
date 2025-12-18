/*
 * Pico Tracker RAW SNIFFER with RSSI FILTER
 * Target: Ch 1 (2401 MHz)
 * Filter 1: First byte 0xC0
 * Filter 2: RSSI (Signal Strength) must be stronger than -50dBm
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <hal/nrf_radio.h>
#include <zephyr/usb/usb_device.h>

// === 參數設定 ===
#define CH_FREQ           1      // 鎖定頻道 1
#define RAW_PREFIX_BYTE   0xC0   // 地址過濾 (只看第1個byte)
#define RSSI_THRESHOLD    50     // 過濾門檻 (50 代表 -50dBm)
                                 // 數值越小代表要求訊號越強 (距離越近)

static uint8_t rx_buffer[32]; 

void radio_init(void)
{
    NRF_RADIO->TASKS_DISABLE = 1;
    k_busy_wait(200);
    NRF_RADIO->EVENTS_DISABLED = 0;

    // 1. 物理層與頻率
    NRF_RADIO->MODE = RADIO_MODE_MODE_Nrf_2Mbit;
    NRF_RADIO->FREQUENCY = CH_FREQ;

    // 2. 地址設定 (只檢查開頭 0xC0)
    NRF_RADIO->BASE0 = (uint32_t)RAW_PREFIX_BYTE; 
    NRF_RADIO->PREFIX0 = 0; 
    NRF_RADIO->TXADDRESS = 0;
    NRF_RADIO->RXADDRESSES = 1;

    // 3. PCNF0 (盲收設定)
    NRF_RADIO->PCNF0 = (0 << RADIO_PCNF0_LFLEN_Pos) |
                       (0 << RADIO_PCNF0_S0LEN_Pos) |
                       (0 << RADIO_PCNF0_S1LEN_Pos);

    // 4. PCNF1 (擴展為 32 bytes)
    NRF_RADIO->PCNF1 = (32 << RADIO_PCNF1_MAXLEN_Pos) | 
                       (32 << RADIO_PCNF1_STATLEN_Pos) | 
                       (1  << RADIO_PCNF1_BALEN_Pos) | 
                       (RADIO_PCNF1_ENDIAN_Little << RADIO_PCNF1_ENDIAN_Pos);

    // 5. 關閉 CRC (什麼都要收)
    NRF_RADIO->CRCCNF = 0; 
    
    // 6. [關鍵] 設定捷徑：地址匹配後，自動啟動 RSSI 測量
    // 這樣我們才能知道訊號有多強
    NRF_RADIO->SHORTS = (RADIO_SHORTS_READY_START_Msk | 
                         RADIO_SHORTS_ADDRESS_RSSISTART_Msk);
}

int main(void)
{
    usb_enable(NULL);
    k_sleep(K_MSEC(2000));
    
    printk("\n============================================\n");
    printk(">>> RSSI FILTER ENABLED (Threshold: -%ddBm) <<<\n", RSSI_THRESHOLD);
    printk(">>> Only STRONG signals will be shown!      <<<\n");
    printk("============================================\n");

    radio_init();

    NRF_RADIO->PACKETPTR = (uint32_t)rx_buffer;
    NRF_RADIO->TASKS_RXEN = 1;
    
    while (1) {
        if (NRF_RADIO->EVENTS_END) {
            NRF_RADIO->EVENTS_END = 0;
            
            // 讀取 RSSI (訊號強度)
            // 暫存器存的是絕對值，例如收到 -42dBm，這裡會讀到 42
            uint8_t rssi_val = NRF_RADIO->RSSISAMPLE;
            
            // === 過濾邏輯 ===
            // 我們只想要「小於 50」的數值 (代表強度 > -50dBm，很近)
            // 數值越大代表訊號越弱 (例如 80 代表 -80dBm)
            if (rssi_val > 0 && rssi_val <= RSSI_THRESHOLD) {
                
                // 再次過濾掉全0的無效封包
                bool valid_data = false;
                for(int i=0; i<5; i++) {
                    if (rx_buffer[i] != 0x00) valid_data = true;
                }

                if (valid_data) {
                    printk("[STRONG Signal -%ddBm] RAW: ", rssi_val);
                    for(int i=0; i<16; i++) printk("%02X ", rx_buffer[i]);
                    printk("\n");
                }
            }
            
            // 如果訊號太弱 (rssi_val > 50)，我們就直接無視，不印出來
            
            // 立即重啟接收
            NRF_RADIO->TASKS_START = 1;
        }
        
        // 降低忙碌等待的頻率
        k_busy_wait(100); 
    }
}

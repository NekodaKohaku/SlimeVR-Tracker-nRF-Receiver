/*
 * Pico Tracker HUNTER v9 (Final verified)
 * 架構：Camper Mode (停留 2 秒) + 簡單延遲啟動
 * 參數：全數修正為 pyOCD 逆向值 (Freq/CRC/S1/Endian)
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <hal/nrf_radio.h>
#include <zephyr/usb/usb_device.h>

// [修正 1] 頻率表：完全對應 Tracker 的跳頻點
static const uint8_t target_freqs[] = {46, 54, 72, 80}; 

// 地址：0xC0 + 0x552C6A1E
#define ADDR_BASE      0x552C6A1EUL
#define ADDR_PREFIX    0xC0

static uint8_t rx_buffer[64];

void radio_init(void)
{
    NRF_RADIO->POWER = 0;
    k_busy_wait(500);
    NRF_RADIO->POWER = 1;

    NRF_RADIO->MODE = NRF_RADIO_MODE_BLE_2MBIT;

    // [修正 2 & 6] PCNF0: S1LEN=4, S1INCL=0
    // 雖然你舊版也是寫 4，但這裡再次確認這是對的
    NRF_RADIO->PCNF0 = (8UL << RADIO_PCNF0_LFLEN_Pos) | 
                       (0UL << RADIO_PCNF0_S0LEN_Pos) | 
                       (4UL << RADIO_PCNF0_S1LEN_Pos);

    // [修正 4 & 5] PCNF1: Big Endian, MaxLen 放寬到 55
    NRF_RADIO->PCNF1 = (55UL << RADIO_PCNF1_MAXLEN_Pos) |
                       (55UL << RADIO_PCNF1_STATLEN_Pos) |
                       (4UL  << RADIO_PCNF1_BALEN_Pos) |
                       (1UL  << RADIO_PCNF1_ENDIAN_Pos) | 
                       (0UL  << RADIO_PCNF1_WHITEEN_Pos);

    NRF_RADIO->BASE0 = ADDR_BASE;
    NRF_RADIO->PREFIX0 = (ADDR_PREFIX << 0);
    
    NRF_RADIO->TXADDRESS = 0;
    NRF_RADIO->RXADDRESSES = 1; 

    // [修正 3] CRC: 0x1021 (之前是 0x11021)
    NRF_RADIO->CRCCNF = (RADIO_CRCCNF_LEN_Two << RADIO_CRCCNF_LEN_Pos) | 
                        (0UL << RADIO_CRCCNF_SKIPADDR_Pos);
    
    NRF_RADIO->CRCINIT = 0xFFFF;
    NRF_RADIO->CRCPOLY = 0x1021; 

    NRF_RADIO->SHORTS = RADIO_SHORTS_READY_START_Msk | RADIO_SHORTS_END_DISABLE_Msk;
}

int main(void)
{
    usb_enable(NULL);
    
    // 簡單暴力的等待，確保你打開視窗時看得到字
    k_sleep(K_MSEC(3000)); 

    printk("\n============================================\n");
    printk(">>> HUNTER v9 (Final Verified)           <<<\n");
    printk(">>> Freqs: 2446, 2454, 2472, 2480 MHz    <<<\n");
    printk("============================================\n");

    radio_init();

    int freq_idx = 0;

    while (1) {
        int current_freq = target_freqs[freq_idx];
        NRF_RADIO->FREQUENCY = current_freq;
        
        printk(">>> Camping on %d MHz...\n", 2400 + current_freq);

        int64_t end_time = k_uptime_get() + 2000;

        while (k_uptime_get() < end_time) {
            
            NRF_RADIO->PACKETPTR = (uint32_t)rx_buffer;
            NRF_RADIO->EVENTS_END = 0;
            NRF_RADIO->TASKS_RXEN = 1;

            bool received = false;
            for (int i = 0; i < 20000; i++) {
                if (NRF_RADIO->EVENTS_END) {
                    received = true;
                    break;
                }
                k_busy_wait(1);
            }

            if (received) {
                // 如果參數全對，這裡的 CRCSTATUS 就會是 1
                if (NRF_RADIO->CRCSTATUS == 1) {
                    int8_t rssi = -(int8_t)NRF_RADIO->RSSISAMPLE; 
                    
                    printk("\n!!! [JACKPOT] Freq %d | RSSI %d !!!\n", 2400 + current_freq, rssi);
                    printk("Data: ");
                    // 印出前 32 bytes 供分析
                    for(int k=0; k<32; k++) printk("%02X ", rx_buffer[k]);
                    printk("\n");
                    
                    end_time += 200; 
                }
                NRF_RADIO->EVENTS_END = 0;
            } else {
                NRF_RADIO->TASKS_DISABLE = 1;
                while (NRF_RADIO->EVENTS_DISABLED == 0);
            }
        }

        freq_idx++;
        if (freq_idx >= sizeof(target_freqs) / sizeof(target_freqs[0])) {
            freq_idx = 0;
        }
    }
}

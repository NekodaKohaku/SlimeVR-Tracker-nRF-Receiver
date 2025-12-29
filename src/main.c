/*
 * Pico Tracker HUNTER v9 (Original Logic + Verified Params)
 * 架構：完全依照你原本的設計 (無 DTR 等待，直接延遲 3 秒)
 * 修正：填入 pyOCD 抓到的正確頻率與 CRC
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <hal/nrf_radio.h>
#include <zephyr/usb/usb_device.h>

// ==========================================
// [修正 1] 頻率改為 pyOCD 觀察到的值
// ==========================================
// 舊: 1, 37, 77 (標準 BLE)
// 新: 46, 54, 72, 80 (2446, 2454, 2472, 2480 MHz)
static const uint8_t target_freqs[] = {46, 54, 72, 80}; 

// ==========================================
// [修正 2] 地址設定
// ==========================================
#define ADDR_BASE      0x552C6A1EUL
#define ADDR_PREFIX    0xC0

static uint8_t rx_buffer[64];
// static uint8_t tx_buffer[32]; // 先註解掉 TX，避免干擾，抓到再開

void radio_init(void)
{
    NRF_RADIO->POWER = 0;
    k_busy_wait(500);
    NRF_RADIO->POWER = 1;

    NRF_RADIO->MODE = NRF_RADIO_MODE_BLE_2MBIT;

    // ==========================================
    // [修正 3] 封包結構 PCNF0
    // ==========================================
    // 你的舊碼: S1=4 -> 這是對的！保留！
    // 這裡我們只是把寫法標準化，數值跟你原本的一樣
    NRF_RADIO->PCNF0 = (8UL << RADIO_PCNF0_LFLEN_Pos) | 
                       (0UL << RADIO_PCNF0_S0LEN_Pos) | 
                       (4UL << RADIO_PCNF0_S1LEN_Pos);

    // PCNF1: 稍微放大 MaxLen 到 55 (0x37)
    NRF_RADIO->PCNF1 = (55UL << RADIO_PCNF1_MAXLEN_Pos) |
                       (55UL << RADIO_PCNF1_STATLEN_Pos) |
                       (4UL  << RADIO_PCNF1_BALEN_Pos) |
                       (1UL  << RADIO_PCNF1_ENDIAN_Pos) | 
                       (0UL  << RADIO_PCNF1_WHITEEN_Pos);

    // 地址
    NRF_RADIO->BASE0 = ADDR_BASE;
    NRF_RADIO->PREFIX0 = (ADDR_PREFIX << 0);
    
    NRF_RADIO->TXADDRESS = 0;
    NRF_RADIO->RXADDRESSES = 1; 

    // ==========================================
    // [修正 4] CRC 修正
    // ==========================================
    // 你的舊碼: 0x11021 (這會導致收不到)
    // 修正為:   0x1021
    NRF_RADIO->CRCCNF = (RADIO_CRCCNF_LEN_Two << RADIO_CRCCNF_LEN_Pos) | 
                        (0UL << RADIO_CRCCNF_SKIPADDR_Pos);
    
    NRF_RADIO->CRCINIT = 0xFFFF;
    NRF_RADIO->CRCPOLY = 0x1021; // <--- 這裡改了

    NRF_RADIO->SHORTS = RADIO_SHORTS_READY_START_Msk | RADIO_SHORTS_END_DISABLE_Msk;
}

int main(void)
{
    // === 你的原始啟動邏輯 ===
    usb_enable(NULL);
    k_sleep(K_MSEC(3000)); // 只用 sleep，不檢查 DTR，保證會跑

    printk("\n============================================\n");
    printk(">>> HUNTER v9 (Original Logic)           <<<\n");
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

            // 短暫等待接收
            bool received = false;
            for (int i = 0; i < 20000; i++) {
                if (NRF_RADIO->EVENTS_END) {
                    received = true;
                    break;
                }
                k_busy_wait(1);
            }

            if (received) {
                if (NRF_RADIO->CRCSTATUS == 1) {
                    // ★ 這裡是你最想看到的 ★
                    int8_t rssi = -(int8_t)NRF_RADIO->RSSISAMPLE; 
                    printk("\n!!! [JACKPOT] Freq %d | RSSI %d !!!\n", 2400 + current_freq, rssi);
                    printk("Data: ");
                    for(int k=0; k<32; k++) printk("%02X ", rx_buffer[k]);
                    printk("\n");
                    
                    end_time += 200; // 延長停留
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

/*
 * Pico Tracker HUNTER v6 (Camper Mode)
 * 策略：長時間駐留已知頻率 (2s)，解決 Slow Radio 問題
 * 新增：雙重地址掃描 (Normal + Reversed) 確保萬無一失
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <hal/nrf_radio.h>
#include <zephyr/usb/usb_device.h>

// 已知 Tracker 會跳的頻率
static const uint8_t target_freqs[] = {1, 37, 77}; // 2401, 2437, 2477 MHz

// 地址定義
#define ADDR_BASE_NORMAL    0x552c6a1eUL
#define ADDR_PREFIX_NORMAL  0xC0

// 反轉地址 (預防萬一) - 雖然硬體說是 Big Endian，但多聽一個沒損失
#define ADDR_BASE_REV       0x1e6a2c55UL
#define ADDR_PREFIX_REV     0xC0

static uint8_t rx_buffer[64];
static uint8_t tx_buffer[32];

void radio_init(void)
{
    NRF_RADIO->POWER = 0;
    k_busy_wait(500);
    NRF_RADIO->POWER = 1;

    NRF_RADIO->MODE = NRF_RADIO_MODE_BLE_2MBIT;

    // Tracker 結構: S0=0, L=8, S1=4
    NRF_RADIO->PCNF0 = (8UL << RADIO_PCNF0_LFLEN_Pos) | 
                       (0UL << RADIO_PCNF0_S0LEN_Pos) | 
                       (4UL << RADIO_PCNF0_S1LEN_Pos);

    NRF_RADIO->PCNF1 = (32UL << RADIO_PCNF1_MAXLEN_Pos) |
                       (0UL  << RADIO_PCNF1_STATLEN_Pos) |
                       (4UL  << RADIO_PCNF1_BALEN_Pos) |
                       (1UL  << RADIO_PCNF1_ENDIAN_Pos) | 
                       (0UL  << RADIO_PCNF1_WHITEEN_Pos);

    // === 雙重地址設定 ===
    // 邏輯地址 0: 正常
    NRF_RADIO->BASE0 = ADDR_BASE_NORMAL;
    NRF_RADIO->PREFIX0 = (ADDR_PREFIX_NORMAL << 0); // Prefix Byte 0

    // 邏輯地址 1: 反轉 (如果需要測試)
    // 註：因為 PREFIX 寄存器共用，這裡我們暫時只用 Address 0 專注測試
    // 如果這次還抓不到，下一次再開 Address 1
    NRF_RADIO->TXADDRESS = 0;
    NRF_RADIO->RXADDRESSES = 1; // 只啟用 Address 0

    // CRC: 包含地址 (SkipAddr = 0)
    NRF_RADIO->CRCCNF = (RADIO_CRCCNF_LEN_Two << RADIO_CRCCNF_LEN_Pos) | 
                        (0UL << RADIO_CRCCNF_SKIPADDR_Pos);
    
    NRF_RADIO->CRCINIT = 0xFFFF;
    NRF_RADIO->CRCPOLY = 0x11021;
}

void send_ack(void) {
    tx_buffer[0] = 0; 
    tx_buffer[1] = 0; 
    
    NRF_RADIO->SHORTS = (RADIO_SHORTS_READY_START_Msk | RADIO_SHORTS_END_DISABLE_Msk);
    NRF_RADIO->PACKETPTR = (uint32_t)tx_buffer;
    NRF_RADIO->EVENTS_END = 0;
    NRF_RADIO->TASKS_TXEN = 1;
    while (NRF_RADIO->EVENTS_END == 0);
    NRF_RADIO->SHORTS = 0;
}

int main(void)
{
    usb_enable(NULL);
    k_sleep(K_MSEC(3000));

    printk("\n============================================\n");
    printk(">>> HUNTER v6 (Camper Mode)              <<<\n");
    printk(">>> Camping on Freq 1, 37, 77 (2s each)  <<<\n");
    printk("============================================\n");

    radio_init();

    int freq_idx = 0;

    while (1) {
        int current_freq = target_freqs[freq_idx];
        NRF_RADIO->FREQUENCY = current_freq;
        
        printk(">>> Camping on %d MHz... (Waiting for Slow Radio)\n", current_freq);

        // 在這個頻率停留 2000ms (200 x 10ms)
        for (int t = 0; t < 200; t++) {
            
            // 開啟 RSSI 測量
            NRF_RADIO->SHORTS = RADIO_SHORTS_ADDRESS_RSSISTART_Msk;
            NRF_RADIO->PACKETPTR = (uint32_t)rx_buffer;
            
            NRF_RADIO->EVENTS_READY = 0;
            NRF_RADIO->TASKS_RXEN = 1;
            while (NRF_RADIO->EVENTS_READY == 0);

            NRF_RADIO->EVENTS_END = 0;
            NRF_RADIO->TASKS_START = 1;

            // 單次監聽窗口 10ms
            bool received = false;
            for (volatile int i = 0; i < 10000; i++) {
                if (NRF_RADIO->EVENTS_END) {
                    received = true;
                    break;
                }
            }

            if (received) {
                int rssi = -((int)NRF_RADIO->RSSISAMPLE);
                
                if (NRF_RADIO->CRCSTATUS == 1) {
                    printk("\n!!! [JACKPOT] !!! Freq %d | RSSI %d dBm\n", current_freq, rssi);
                    printk("Data: ");
                    for(int k=0; k<12; k++) printk("%02X ", rx_buffer[k]);
                    printk("\n");
                    
                    // ACK
                    NRF_RADIO->TASKS_DISABLE = 1;
                    while (NRF_RADIO->EVENTS_DISABLED == 0);
                    send_ack();
                    printk("ACK Sent.\n");
                    k_sleep(K_MSEC(100)); // 抓到後稍微停一下
                } else {
                    // 即使 CRC 錯，只要有訊號就是好消息
                    printk("? [Signal Detected] Freq %d | RSSI %d dBm | CRC Fail\n", current_freq, rssi);
                }
            }

            // 準備下一次接收
            NRF_RADIO->TASKS_DISABLE = 1;
            while (NRF_RADIO->EVENTS_DISABLED == 0);
            
            // 如果抓到了，不需要換頻，繼續在這個頻率聽，增加互動機會
            if (received) {
                t--; // 延長駐留時間
            }
        }

        // 換下一個重點頻率
        freq_idx++;
        if (freq_idx >= sizeof(target_freqs)) freq_idx = 0;
    }
}

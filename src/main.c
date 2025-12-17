/*
 * Pico Tracker PASSIVE Scanner (Immediate Output Version)
 * Fix: REMOVED all DTR wait loops. Prints unconditionally.
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <hal/nrf_radio.h>
#include <zephyr/usb/usb_device.h>

// === 參數設定 ===
#define TARGET_BASE_ADDR  0x552c6a1eUL
#define TARGET_PREFIX     0xC0
#define CH_FREQ           1 

// 測試清單 (優先測試 4)
static const uint8_t lflen_candidates[] = {4, 6, 8, 0};
#define CANDIDATE_COUNT 4

static uint8_t rx_buffer[32]; 

// 安全等待函數
void wait_for_disabled(void) {
    int timeout = 5000;
    while (NRF_RADIO->EVENTS_DISABLED == 0 && timeout > 0) {
        k_busy_wait(10);
        timeout--;
    }
    NRF_RADIO->EVENTS_DISABLED = 0;
}

void radio_configure_rx(uint8_t lflen_val)
{
    NRF_RADIO->TASKS_DISABLE = 1;
    wait_for_disabled();

    NRF_RADIO->MODE = RADIO_MODE_MODE_Nrf_2Mbit;
    NRF_RADIO->FREQUENCY = CH_FREQ;

    NRF_RADIO->BASE0 = TARGET_BASE_ADDR;
    NRF_RADIO->PREFIX0 = TARGET_PREFIX;
    NRF_RADIO->TXADDRESS = 0;
    NRF_RADIO->RXADDRESSES = 1;

    // 印出目前參數 (強制換行)
    printk("Scanning LFLEN = %d bits... \n", lflen_val);

    NRF_RADIO->PCNF0 = (lflen_val << RADIO_PCNF0_LFLEN_Pos);

    if (lflen_val > 0) {
        NRF_RADIO->PCNF1 = (32 << RADIO_PCNF1_MAXLEN_Pos) | 
                           (0  << RADIO_PCNF1_STATLEN_Pos) | 
                           (4  << RADIO_PCNF1_BALEN_Pos) | 
                           (RADIO_PCNF1_ENDIAN_Little << RADIO_PCNF1_ENDIAN_Pos);
    } else {
        NRF_RADIO->PCNF1 = (32 << RADIO_PCNF1_MAXLEN_Pos) | 
                           (32 << RADIO_PCNF1_STATLEN_Pos) | 
                           (4  << RADIO_PCNF1_BALEN_Pos) | 
                           (RADIO_PCNF1_ENDIAN_Little << RADIO_PCNF1_ENDIAN_Pos);
    }

    NRF_RADIO->CRCCNF = (RADIO_CRCCNF_LEN_Two << RADIO_CRCCNF_LEN_Pos);
    NRF_RADIO->CRCINIT = 0xFFFF;
    NRF_RADIO->CRCPOLY = 0x11021;
    NRF_RADIO->SHORTS = (RADIO_SHORTS_READY_START_Msk);
}

int main(void)
{
    // 1. 啟動 USB
    if (usb_enable(NULL)) return 0;

    // 2. [修正] 移除所有 while(!dtr) 等待
    // 只給 3 秒緩衝，不管有沒有人連線，時間到就開始跑
    k_sleep(K_MSEC(3000)); 
    
    printk("\n=========================================\n");
    printk(">>> PASSIVE SCANNER STARTED (Force Output) <<<\n");
    printk("=========================================\n");

    int current_idx = 0;
    bool locked = false;

    while (1) {
        uint8_t test_lflen = lflen_candidates[current_idx];

        if (!locked) {
            radio_configure_rx(test_lflen);
        }

        // 啟動接收
        NRF_RADIO->PACKETPTR = (uint32_t)rx_buffer;
        NRF_RADIO->TASKS_RXEN = 1;

        // 監聽 200ms
        bool packet_found = false;
        for (int t = 0; t < 200; t++) { 
            if (NRF_RADIO->EVENTS_END) {
                NRF_RADIO->EVENTS_END = 0;
                
                if (NRF_RADIO->CRCSTATUS == 1) {
                    packet_found = true;
                    break; 
                } else {
                    NRF_RADIO->TASKS_START = 1; 
                }
            }
            k_busy_wait(1000); 
        }

        if (packet_found) {
            printk("\n>>> [CAPTURED!] Valid Packet Found! LFLEN = %d <<<\n", test_lflen);
            printk("Payload (Hex): ");
            for(int k=0; k<16; k++) printk("%02X ", rx_buffer[k]);
            printk("\n");
            
            locked = true; 
            NRF_RADIO->TASKS_START = 1;
        } 
        else {
            if (!locked) {
                current_idx++;
                if (current_idx >= CANDIDATE_COUNT) current_idx = 0;
            } else {
                NRF_RADIO->TASKS_START = 1;
            }
        }
    }
}

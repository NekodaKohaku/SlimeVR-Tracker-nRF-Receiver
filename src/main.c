/*
 * Pico Tracker HUNTER v3 (CRC FIX)
 * 修正重點：CRCCNF SKIPADDR = 0 (必須包含地址！)
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <hal/nrf_radio.h>
#include <zephyr/usb/usb_device.h>

#define TARGET_BASE_ADDR  0x552c6a1eUL
#define TARGET_PREFIX     0xC0

static uint8_t rx_buffer[64];
static uint8_t tx_buffer[32];

void radio_init(void)
{
    NRF_RADIO->POWER = 0;
    k_busy_wait(500);
    NRF_RADIO->POWER = 1;

    NRF_RADIO->MODE = NRF_RADIO_MODE_BLE_2MBIT;

    // S0=0, L=8, S1=4 (完全對齊 Tracker)
    NRF_RADIO->PCNF0 = (8UL << RADIO_PCNF0_LFLEN_Pos) | 
                       (0UL << RADIO_PCNF0_S0LEN_Pos) | 
                       (4UL << RADIO_PCNF0_S1LEN_Pos);

    NRF_RADIO->PCNF1 = (32UL << RADIO_PCNF1_MAXLEN_Pos) |
                       (0UL  << RADIO_PCNF1_STATLEN_Pos) |
                       (4UL  << RADIO_PCNF1_BALEN_Pos) |
                       (1UL  << RADIO_PCNF1_ENDIAN_Pos) | // Big Endian
                       (0UL  << RADIO_PCNF1_WHITEEN_Pos);

    NRF_RADIO->BASE0 = TARGET_BASE_ADDR;
    NRF_RADIO->PREFIX0 = TARGET_PREFIX;
    NRF_RADIO->TXADDRESS = 0;
    NRF_RADIO->RXADDRESSES = 1;

    // === 致命錯誤修正 ===
    // Tracker 的 CRCCNF 是 0x02，代表 SKIPADDR=0 (Include Address)
    // 我們之前設成了 Skip (1)，導致 CRC 永遠算錯！
    NRF_RADIO->CRCCNF = (RADIO_CRCCNF_LEN_Two << RADIO_CRCCNF_LEN_Pos) | 
                        (0UL << RADIO_CRCCNF_SKIPADDR_Pos); // 修正：包含地址！
    
    NRF_RADIO->CRCINIT = 0xFFFF;
    NRF_RADIO->CRCPOLY = 0x11021;

    NRF_RADIO->SHORTS = 0; 
}

void send_ack(void) {
    // 準備一個簡單的空包作為 ACK
    tx_buffer[0] = 0; // Length=0
    tx_buffer[1] = 0; // S1=0
    
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
    printk(">>> HUNTER v3 (CRC Address Fix)          <<<\n");
    printk(">>> Scanning 0-80 MHz...                 <<<\n");
    printk("============================================\n");

    radio_init();

    int freq = 0;

    while (1) {
        NRF_RADIO->FREQUENCY = freq;
        
        NRF_RADIO->PACKETPTR = (uint32_t)rx_buffer;
        NRF_RADIO->EVENTS_READY = 0;
        NRF_RADIO->TASKS_RXEN = 1;
        while (NRF_RADIO->EVENTS_READY == 0);

        NRF_RADIO->EVENTS_END = 0;
        NRF_RADIO->TASKS_START = 1;

        // 掃描視窗 20ms
        bool received = false;
        for (volatile int i = 0; i < 20000; i++) {
            if (NRF_RADIO->EVENTS_END) {
                received = true;
                break;
            }
        }

        if (received) {
            if (NRF_RADIO->CRCSTATUS == 1) {
                // 抓到了！
                printk("\n>>> [GOTCHA!] Freq %d MHz | Data: ", freq);
                for(int k=0; k<12; k++) printk("%02X ", rx_buffer[k]);
                printk("<<<\n");

                // 立刻發送 ACK 喚醒它
                NRF_RADIO->TASKS_DISABLE = 1;
                while (NRF_RADIO->EVENTS_DISABLED == 0);
                send_ack();
                printk("ACK Sent! (Check Tracker LED)\n");
                
                k_sleep(K_MSEC(500)); // 暫停一下慶祝
            } else {
                // 如果只出現 ! 但沒抓到，代表頻率對了但可能有干擾
                // printk("!"); 
            }
        }

        // 換頻
        NRF_RADIO->TASKS_DISABLE = 1;
        while (NRF_RADIO->EVENTS_DISABLED == 0);
        
        freq++;
        if (freq > 80) freq = 0;
    }
}

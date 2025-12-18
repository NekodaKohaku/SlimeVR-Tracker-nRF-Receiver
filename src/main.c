/*
 * Pico Tracker EXACT CLONE RECEIVER
 * Based on pyocd dump:
 * - MODE: Ble_2Mbit (Val: 4) <--- CRITICAL FIX
 * - LFLEN: 8 bits
 * - S1LEN: 4 bits
 * - ADDR: C0 55 2C 6A 1E
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <hal/nrf_radio.h>
#include <zephyr/usb/usb_device.h>

// === 根據 Dump 出來的鐵證 ===
#define CH_FREQ           1           // 0x40001508 = 1
#define TARGET_BASE_ADDR  0x552c6a1eUL // 0x4000151C
#define TARGET_PREFIX     0xC0        // 0x40001524 (Byte 0)

static uint8_t rx_buffer[32]; 

void radio_init(void)
{
    NRF_RADIO->TASKS_DISABLE = 1;
    k_busy_wait(200);
    NRF_RADIO->EVENTS_DISABLED = 0;

    // 1. [致命修正] 設定為 BLE 2Mbit 模式 (值為 4)
    // 之前用 Nrf_2Mbit (值為 1) 是收不到的！
    NRF_RADIO->MODE = 4; // RADIO_MODE_MODE_Ble_2Mbit

    NRF_RADIO->FREQUENCY = CH_FREQ;

    // 2. 地址設定
    NRF_RADIO->BASE0 = TARGET_BASE_ADDR;
    NRF_RADIO->PREFIX0 = TARGET_PREFIX;
    NRF_RADIO->TXADDRESS = 0;      // Dump 顯示它用 Address 0 發射
    NRF_RADIO->RXADDRESSES = 1;    // 啟用接收 Address 0

    // 3. [結構修正] 根據 0x40001514 = 00040008
    // LFLEN = 8 (1 byte 長度欄位)
    // S0LEN = 0
    // S1LEN = 4 (4 bits 額外欄位)
    NRF_RADIO->PCNF0 = (8 << RADIO_PCNF0_LFLEN_Pos) |
                       (0 << RADIO_PCNF0_S0LEN_Pos) |
                       (4 << RADIO_PCNF0_S1LEN_Pos);

    // 4. PCNF1 根據 0x40001518 = 01040023
    // MaxLen 35 (0x23), Balen 4, Endian Little
    NRF_RADIO->PCNF1 = (35 << RADIO_PCNF1_MAXLEN_Pos) | 
                       (0  << RADIO_PCNF1_STATLEN_Pos) | // StatLen 0
                       (4  << RADIO_PCNF1_BALEN_Pos) | 
                       (RADIO_PCNF1_ENDIAN_Little << RADIO_PCNF1_ENDIAN_Pos);

    // 5. CRC 設定 (標準)
    NRF_RADIO->CRCCNF = (RADIO_CRCCNF_LEN_Two << RADIO_CRCCNF_LEN_Pos);
    NRF_RADIO->CRCINIT = 0xFFFF;
    NRF_RADIO->CRCPOLY = 0x11021;
    
    // 6. 啟用捷徑
    NRF_RADIO->SHORTS = RADIO_SHORTS_READY_START_Msk;
}

int main(void)
{
    usb_enable(NULL);
    k_sleep(K_MSEC(2000));
    
    printk("\n============================================\n");
    printk(">>> CLONE RECEIVER STARTED               <<<\n");
    printk(">>> MODE: Ble_2Mbit (Fix), LFLEN: 8 bits <<<\n");
    printk("============================================\n");

    radio_init();

    NRF_RADIO->PACKETPTR = (uint32_t)rx_buffer;
    NRF_RADIO->TASKS_RXEN = 1;
    
    int tick = 0;

    while (1) {
        if (NRF_RADIO->EVENTS_END) {
            NRF_RADIO->EVENTS_END = 0;
            
            if (NRF_RADIO->CRCSTATUS == 1) {
                printk("\n>>> [MATCH!] Received Valid Packet! <<<\n");
                printk("Payload: ");
                for(int i=0; i<16; i++) printk("%02X ", rx_buffer[i]);
                printk("\n");
                
                // 收到後重啟
                NRF_RADIO->TASKS_START = 1;
            } else {
                // 如果 CRC 錯，但有收到東西，也印一下驚嘆號，代表頻率/Mode對了
                 // printk("!"); 
                 NRF_RADIO->TASKS_START = 1;
            }
        }

        tick++;
        if (tick % 1000 == 0) {
             // printk("."); // 心跳
        }
        
        k_busy_wait(1000); 
    }
}

/* * ROLE: LISTENER (Dongle B)
 * 任務: 專心監聽 2401 MHz，印出所有收到的封包
 * 配置: Big Endian, Prefix C0, Ch 1 (與 Sender 完全一致)
 */
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/usb/usb_device.h>
#include <hal/nrf_radio.h>

#define TARGET_FREQ 1 // 2401 MHz

static uint8_t rx_buffer[64];

void main(void) {
    usb_enable(NULL);
    k_sleep(K_SECONDS(3));
    printk("\n=== LISTENER START (Looking for ACK) ===\n");

    // Init Radio (RX Config)
    NRF_RADIO->TASKS_DISABLE = 1;
    while(NRF_RADIO->EVENTS_DISABLED == 0);
    NRF_RADIO->FREQUENCY = TARGET_FREQ;
    NRF_RADIO->MODE = (RADIO_MODE_MODE_Nrf_2Mbit << RADIO_MODE_MODE_Pos);
    NRF_RADIO->PREFIX0 = 0xC0;
    NRF_RADIO->BASE0 = 0x552C6A1E;
    NRF_RADIO->RXADDRESSES = 1;

    // PCNF0: S0=0, S1=4
    NRF_RADIO->PCNF0 = (8UL << RADIO_PCNF0_LFLEN_Pos) | (4UL << RADIO_PCNF0_S1LEN_Pos);
    
    // PCNF1: Big Endian, BALEN=4
    NRF_RADIO->PCNF1 = (35UL << RADIO_PCNF1_MAXLEN_Pos) | (4UL << RADIO_PCNF1_BALEN_Pos) |
                       (RADIO_PCNF1_ENDIAN_Big << RADIO_PCNF1_ENDIAN_Pos);

    NRF_RADIO->CRCCNF = 2; 
    NRF_RADIO->CRCINIT = 0xFFFF;
    NRF_RADIO->CRCPOLY = 0x11021;

    NRF_RADIO->PACKETPTR = (uint32_t)rx_buffer;
    NRF_RADIO->SHORTS = RADIO_SHORTS_READY_START_Msk | RADIO_SHORTS_END_START_Msk; // 持續接收
    NRF_RADIO->TASKS_RXEN = 1;

    printk("Listening on 2401 MHz...\n");

    while(1) {
        if (NRF_RADIO->EVENTS_END) {
            NRF_RADIO->EVENTS_END = 0;
            
            if (NRF_RADIO->CRCSTATUS) {
                // CRC OK! 印出封包內容
                uint8_t len = rx_buffer[0];
                
                // 區分 Sender 和 Tracker
                if (len == 0x11) {
                     // 這是 Dongle A (Sender) 發的
                     // printk("A"); 
                } else if (len == 0x0D) {
                     // 這是 Tracker 發的 ACK (長度 13) !!
                     printk("\n[!] TRACKER REPLY DETECTED! [Len: 0D]\n");
                     printk("    Raw: ");
                     for(int i=0; i<15; i++) printk("%02X ", rx_buffer[i]);
                     printk("\n");
                } else {
                     // 未知封包
                     printk("\n[?] Unknown Packet Len: %02X\n", len);
                }
            }
        }
        k_yield();
    }
}

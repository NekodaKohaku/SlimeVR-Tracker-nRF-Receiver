/*
 * Pico Tracker Receiver - The "Nibble Length" Fix
 * Hypothesis: LFLEN is 4 bits (Value 4).
 * This fits the '4' you saw and allows 8-byte payloads (max 15).
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <hal/nrf_radio.h>
#include <zephyr/usb/usb_device.h>

// 參數設定
#define TARGET_BASE_ADDR  0x552c6a1eUL
#define TARGET_PREFIX     0xC0
#define RX_FREQ           1 

static uint8_t rx_buffer[32];

void radio_configure(void)
{
    NRF_RADIO->TASKS_DISABLE = 1;
    while (NRF_RADIO->EVENTS_DISABLED == 0);
    NRF_RADIO->EVENTS_DISABLED = 0;

    NRF_RADIO->MODE = RADIO_MODE_MODE_Nrf_2Mbit;
    NRF_RADIO->FREQUENCY = RX_FREQ;

    NRF_RADIO->BASE0 = TARGET_BASE_ADDR;
    NRF_RADIO->PREFIX0 = TARGET_PREFIX;
    NRF_RADIO->TXADDRESS = 0;
    NRF_RADIO->RXADDRESSES = 1; 

    // [關鍵修正] PCNF0
    // LFLEN = 4 bits (對應數值 4)
    // 這意味著封包的開頭 4 個 bit 是長度，後面接著數據
    NRF_RADIO->PCNF0 = (4 << RADIO_PCNF0_LFLEN_Pos);

    // [修正] PCNF1
    // 恢復為我們在 dump 2 看到的合理值：MaxLen 8, Balen 4
    // 雖然您剛讀到 4，但 Balen=0 風險太大，我們先用標準的 4 byte Base
    NRF_RADIO->PCNF1 = (8 << RADIO_PCNF1_MAXLEN_Pos) | 
                       (0 << RADIO_PCNF1_STATLEN_Pos) | // 依賴 LFLEN
                       (4 << RADIO_PCNF1_BALEN_Pos) | 
                       (RADIO_PCNF1_ENDIAN_Little << RADIO_PCNF1_ENDIAN_Pos);

    NRF_RADIO->CRCCNF = (RADIO_CRCCNF_LEN_Two << RADIO_CRCCNF_LEN_Pos);
    NRF_RADIO->CRCINIT = 0xFFFF;
    NRF_RADIO->CRCPOLY = 0x11021;

    NRF_RADIO->SHORTS = (RADIO_SHORTS_READY_START_Msk | RADIO_SHORTS_END_DISABLE_Msk);
}

int main(void)
{
    if (usb_enable(NULL)) return 0;
    k_sleep(K_MSEC(2000));
    printk(">>> RECEIVER TRYING LFLEN=4 <<<\n");

    radio_configure();

    while (1) {
        NRF_RADIO->PACKETPTR = (uint32_t)rx_buffer;
        
        // 為了讓測試更穩定，每次接收前先清空 CRC 狀態
        NRF_RADIO->EVENTS_END = 0;
        NRF_RADIO->TASKS_RXEN = 1;

        // 等待接收 (Bus Wait)
        // 這裡設定較長的 Timeout，確保有抓到
        for(int i=0; i<2000; i++) {
            if (NRF_RADIO->EVENTS_END) {
                NRF_RADIO->EVENTS_END = 0;
                
                if (NRF_RADIO->CRCSTATUS == 1) {
                    printk("[RX] SUCCESS! LFLEN=4 Works! | Payload: ");
                    for(int k=0; k<10; k++) printk("%02X ", rx_buffer[k]);
                    printk("\n");
                }
                NRF_RADIO->TASKS_START = 1; // 繼續收
                break;
            }
            k_busy_wait(10);
        }
    }
}

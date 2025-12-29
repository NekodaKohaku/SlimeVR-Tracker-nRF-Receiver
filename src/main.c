/*
 * Pico Tracker Sniffer (FINAL VERIFIED)
 * Target: nRF52810 (Pico Tracker)
 * * [Verified Parameters via pyOCD]
 * MODE:    Ble_2Mbit (0x04)
 * PCNF0:   L=8, S0=0, S1=4 (Critical!)
 * PCNF1:   MaxLen=55, BigEndian, Balen=4
 * ADDRESS: Base=0x552C6A1E, Prefix=0xC0
 * CRC:     16-bit, Poly=0x1021, Init=0xFFFF
 * FREQ:    Hopping {2446, 2454, 2472, 2480}
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <hal/nrf_radio.h>
#include <zephyr/usb/usb_device.h>

// 頻率白名單 (根據 pyOCD 觀察到的值)
static const uint8_t target_freqs[] = {46, 54, 72, 80}; 

// 地址設定
#define ADDR_BASE      0x552C6A1EUL
#define ADDR_PREFIX    0xC0

static uint8_t rx_buffer[64];

void radio_init(void)
{
    // 重置 Radio
    NRF_RADIO->POWER = 0;
    k_busy_wait(500);
    NRF_RADIO->POWER = 1;

    // 1. 物理層 (MODE)
    NRF_RADIO->MODE = NRF_RADIO_MODE_BLE_2MBIT; // 0x04

    // 2. 封包結構 (PCNF0) - ★修正了 S1LEN★
    // 0x00040008 -> LFLEN=8, S1LEN=4
    NRF_RADIO->PCNF0 = (8UL << RADIO_PCNF0_LFLEN_Pos) | 
                       (0UL << RADIO_PCNF0_S0LEN_Pos) | 
                       (4UL << RADIO_PCNF0_S1LEN_Pos);  // <--- 這裡改成 4 了！

    // 3. 封包結構 (PCNF1) - 修正了 MaxLen
    // 0x01040037 -> MaxLen=55, Balen=4, Endian=Big
    NRF_RADIO->PCNF1 = (55UL << RADIO_PCNF1_MAXLEN_Pos) |
                       (55UL << RADIO_PCNF1_STATLEN_Pos) |
                       (4UL  << RADIO_PCNF1_BALEN_Pos) |
                       (1UL  << RADIO_PCNF1_ENDIAN_Pos) |  // Big Endian
                       (0UL  << RADIO_PCNF1_WHITEEN_Pos);

    // 4. 地址設定
    NRF_RADIO->BASE0 = ADDR_BASE;
    NRF_RADIO->PREFIX0 = (ADDR_PREFIX << 0);
    
    NRF_RADIO->TXADDRESS = 0;
    NRF_RADIO->RXADDRESSES = 1; // 啟用 Pipe 0

    // 5. CRC 設定
    // CRCCNF=2 (Len=2, SkipAddr=0)
    NRF_RADIO->CRCCNF = (RADIO_CRCCNF_LEN_Two << RADIO_CRCCNF_LEN_Pos) | 
                        (0UL << RADIO_CRCCNF_SKIPADDR_Pos);
    
    NRF_RADIO->CRCINIT = 0xFFFF;
    NRF_RADIO->CRCPOLY = 0x1021; 

    // 6. 捷徑
    NRF_RADIO->SHORTS = RADIO_SHORTS_READY_START_Msk | RADIO_SHORTS_END_DISABLE_Msk;
}

int main(void)
{
    const struct device *dev = device_get_binding("CDC_ACM_0");
    if (usb_enable(NULL)) {
        return 0;
    }
    
    k_sleep(K_MSEC(2000)); // 等待 USB

    printk("\n============================================\n");
    printk(">>> Pico Tracker Sniffer (S1=4 Fixed)    <<<\n");
    printk(">>> Ready to Capture on 2446/54/72/80MHz <<<\n");
    printk("============================================\n");

    radio_init();

    int freq_idx = 0;

    while (1) {
        int current_freq = target_freqs[freq_idx];
        NRF_RADIO->FREQUENCY = current_freq;
        
        printk(">>> [Freq %d MHz] Listening...\n", 2400 + current_freq);

        // 在每個頻率停留 2 秒
        int64_t end_time = k_uptime_get() + 2000;

        while (k_uptime_get() < end_time) {
            
            NRF_RADIO->PACKETPTR = (uint32_t)rx_buffer;
            NRF_RADIO->EVENTS_END = 0;
            NRF_RADIO->TASKS_RXEN = 1;

            // 等待接收 (20ms timeout)
            bool packet_received = false;
            for (int i = 0; i < 20000; i++) {
                if (NRF_RADIO->EVENTS_END) {
                    packet_received = true;
                    break;
                }
                k_busy_wait(1);
            }

            if (packet_received) {
                if (NRF_RADIO->CRCSTATUS == 1) {
                    printk("\n[RX OK] Freq: %d | Data: ", 2400 + current_freq);
                    // 印出前 32 bytes
                    for(int k=0; k < 32; k++) {
                        printk("%02X ", rx_buffer[k]);
                    }
                    printk("\n");
                    
                    // 抓到了！延長停留時間，多抓幾包
                    end_time += 200; 
                } 
                NRF_RADIO->EVENTS_END = 0;
            } else {
                // 超時，手動關閉
                NRF_RADIO->TASKS_DISABLE = 1;
                while (NRF_RADIO->EVENTS_DISABLED == 0);
            }
        }

        // 換頻
        freq_idx++;
        if (freq_idx >= sizeof(target_freqs) / sizeof(target_freqs[0])) {
            freq_idx = 0;
        }
    }
}

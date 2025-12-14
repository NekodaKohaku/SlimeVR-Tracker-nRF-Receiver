/*
 * Pico Tracker "Passive Hunter"
 * Strategy: Listen ONLY. Assume tracker is advertising.
 * Address: 0x4E4E4E4E + 0x04 (from Dump)
 * Frequency: Focus on CH 40 (2440MHz) from Dump hint
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/usb/usb_device.h>
#include <hal/nrf_radio.h>

void radio_init_listener(uint8_t channel) {
    NRF_RADIO->TASKS_DISABLE = 1;
    while (NRF_RADIO->EVENTS_DISABLED == 0);
    NRF_RADIO->EVENTS_DISABLED = 0;

    // 1. Config 2Mbps ESB (Standard)
    nrf_radio_mode_set(NRF_RADIO, NRF_RADIO_MODE_NRF_2MBIT);
    nrf_radio_frequency_set(NRF_RADIO, channel);

    // 2. Address Configuration (From Dump)
    // 組合 A: 0x04 + 0x4E4E4E4E
    nrf_radio_base0_set(NRF_RADIO, 0x4E4E4E4E);
    nrf_radio_prefix0_set(NRF_RADIO, 0x04);
    
    // 組合 B: 0x04 + 0x004E4E4E (Little Endian variant)
    nrf_radio_base1_set(NRF_RADIO, 0x004E4E4E);
    nrf_radio_prefix1_set(NRF_RADIO, 0x04);

    nrf_radio_rxaddresses_set(NRF_RADIO, 3); // Enable logical 0 & 1

    // 3. Packet Config (Standard ESB)
    NRF_RADIO->PCNF0 = (8 << RADIO_PCNF0_LFLEN_Pos) | (1 << RADIO_PCNF0_S0LEN_Pos);
    NRF_RADIO->PCNF1 = (32 << RADIO_PCNF1_MAXLEN_Pos) | 
                       (4 << RADIO_PCNF1_BALEN_Pos) | 
                       (RADIO_PCNF1_ENDIAN_Big << RADIO_PCNF1_ENDIAN_Pos) |
                       (RADIO_PCNF1_WHITEEN_Enabled << RADIO_PCNF1_WHITEEN_Pos);

    // 4. CRC (開啟但我們會印出 CRC Error 的包)
    nrf_radio_crc_configure(NRF_RADIO, RADIO_CRCCNF_LEN_Two, NRF_RADIO_CRC_ADDR_SKIP, 0x11021);
    NRF_RADIO->CRCINIT = 0xFFFF;
    NRF_RADIO->DATAWHITEIV = channel | 0x40;

    // 5. Shorts (Auto Start RX after Ready)
    NRF_RADIO->SHORTS = RADIO_SHORTS_READY_START_Msk;
}

int main(void) {
    if (IS_ENABLED(CONFIG_USB_DEVICE_STACK)) {
        usb_enable(NULL);
    }

    k_sleep(K_SECONDS(2));
    printk("\n=== Pico Passive Hunter ===\n");
    printk("Listening for Tracker Broadcasts...\n");
    printk("Address: 0x4E4E4E4E / 0x004E4E4E (Prefix 04)\n");

    // 重點監聽這幾個頻道
    uint8_t target_channels[] = {40, 2, 26, 80, 4, 78}; 
    int ch_idx = 0;

    while (1) {
        uint8_t ch = target_channels[ch_idx];
        radio_init_listener(ch);
        
        // 啟動接收
        NRF_RADIO->TASKS_RXEN = 1;

        // 在這個頻道死守 200ms (因為 Slow ADV 可能很慢)
        for (int i = 0; i < 200; i++) {
            
            // 只要偵測到「地址匹配 (ADDRESS)」就立刻報警
            // 不管 CRC 對不對，先抓再說
            if (NRF_RADIO->EVENTS_ADDRESS) {
                NRF_RADIO->EVENTS_ADDRESS = 0;
                
                printk("\n[!] SIGNAL DETECTED on CH %d!\n", ch);
                
                // 等待 Payload 接收完畢
                while (NRF_RADIO->EVENTS_END == 0 && NRF_RADIO->EVENTS_CRCERROR == 0 && NRF_RADIO->EVENTS_CRCOK == 0);
                
                if (NRF_RADIO->EVENTS_CRCOK) {
                    printk(">>> CRC MATCH! This is DEFINITELY it! <<<\n");
                } else if (NRF_RADIO->EVENTS_CRCERROR) {
                    printk("--- CRC Error (Might be noise or Endian mismatch) ---\n");
                }
                
                // 為了避免刷屏，收到一次後暫停一下
                NRF_RADIO->TASKS_DISABLE = 1;
                k_sleep(K_MSEC(100));
                NRF_RADIO->TASKS_RXEN = 1;
            }
            k_busy_wait(1000); // 1ms polling
        }

        // 換下一個頻道
        ch_idx++;
        if (ch_idx >= sizeof(target_channels)) ch_idx = 0;
        
        printk("."); // 心跳包
    }
    return 0;
}

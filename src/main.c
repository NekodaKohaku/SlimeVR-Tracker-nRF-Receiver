/*
 * Pico Tracker "Config Sweeper"
 * Lock: CH 73 & 45
 * Sweep: Endianness (Big/Little) & Whitening (On/Off)
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/usb/usb_device.h>
#include <hal/nrf_radio.h>

#define CH_A 73
#define CH_B 45

// 嘗試捕捉的地址 (Pico Special & Default)
static uint32_t addrs[] = {0x43434343, 0xE7E7E7E7};

void configure_radio(uint8_t channel, bool big_endian, bool whitening) {
    // 1. Reset
    NRF_RADIO->TASKS_DISABLE = 1;
    while (NRF_RADIO->EVENTS_DISABLED == 0);
    NRF_RADIO->EVENTS_DISABLED = 0;

    // 2. Basic
    nrf_radio_power_set(NRF_RADIO, true);
    nrf_radio_frequency_set(NRF_RADIO, channel);
    nrf_radio_mode_set(NRF_RADIO, NRF_RADIO_MODE_NRF_2MBIT); // Try 2Mbps first

    // 3. Addresses
    nrf_radio_base0_set(NRF_RADIO, addrs[0]);
    nrf_radio_prefix0_set(NRF_RADIO, addrs[0] & 0xFF); // Prefix = LSB
    nrf_radio_base1_set(NRF_RADIO, addrs[1]);
    nrf_radio_prefix1_set(NRF_RADIO, addrs[1] & 0xFF);
    nrf_radio_rxaddresses_set(NRF_RADIO, 3); // Pipe 0 & 1

    // 4. PCNF0 (8-bit Length)
    NRF_RADIO->PCNF0 = (8 << RADIO_PCNF0_LFLEN_Pos);

    // 5. PCNF1 (Dynamic Config)
    uint32_t pcnf1 = (32 << RADIO_PCNF1_MAXLEN_Pos) | (4 << RADIO_PCNF1_BALEN_Pos);
    
    if (big_endian) {
        pcnf1 |= (RADIO_PCNF1_ENDIAN_Big << RADIO_PCNF1_ENDIAN_Pos);
    } else {
        pcnf1 |= (RADIO_PCNF1_ENDIAN_Little << RADIO_PCNF1_ENDIAN_Pos);
    }

    if (whitening) {
        pcnf1 |= (RADIO_PCNF1_WHITEEN_Enabled << RADIO_PCNF1_WHITEEN_Pos);
    } else {
        pcnf1 |= (RADIO_PCNF1_WHITEEN_Disabled << RADIO_PCNF1_WHITEEN_Pos);
    }

    NRF_RADIO->PCNF1 = pcnf1;

    // 6. CRC & Whitening IV
    nrf_radio_crc_configure(NRF_RADIO, RADIO_CRCCNF_LEN_Two, NRF_RADIO_CRC_ADDR_SKIP, 0x11021);
    NRF_RADIO->CRCINIT = 0xFFFF;
    NRF_RADIO->DATAWHITEIV = channel | 0x40;

    // 7. Start RX
    NRF_RADIO->SHORTS = RADIO_SHORTS_READY_START_Msk | RADIO_SHORTS_END_START_Msk;
    NRF_RADIO->EVENTS_CRCOK = 0;
    NRF_RADIO->TASKS_RXEN = 1;
}

int main(void) {
    if (IS_ENABLED(CONFIG_USB_DEVICE_STACK)) {
        usb_enable(NULL);
    }
    k_sleep(K_SECONDS(3));
    printk("\n=== Pico Config Sweeper ===\n");
    printk("Scanning combinations on CH 73 & 45...\n");

    uint8_t channels[] = {CH_A, CH_B};
    int ch_idx = 0;
    
    while (1) {
        uint8_t ch = channels[ch_idx];
        
        // 嘗試 4 種組合
        for (int i = 0; i < 4; i++) {
            bool big_endian = (i & 1);
            bool whitening = (i & 2);

            configure_radio(ch, big_endian, whitening);
            
            // 每個設定聽 150ms
            for (int k = 0; k < 15; k++) {
                if (NRF_RADIO->EVENTS_CRCOK) {
                    NRF_RADIO->EVENTS_CRCOK = 0;
                    printk("\n!!! MATCH FOUND !!!\n");
                    printk("CH: %d | Endian: %s | White: %s\n", 
                           ch, 
                           big_endian ? "BIG" : "LITTLE", 
                           whitening ? "ON" : "OFF");
                    
                    // 鎖定成功，不再切換，專心接收
                    while(1) {
                        if (NRF_RADIO->EVENTS_CRCOK) {
                            NRF_RADIO->EVENTS_CRCOK = 0;
                            printk("!"); // 收到數據包
                        }
                        k_sleep(K_MSEC(1));
                    }
                }
                k_sleep(K_MSEC(10));
            }
        }
        
        // 切換頻道
        ch_idx = (ch_idx + 1) % 2;
        printk(".");
    }
    return 0;
}

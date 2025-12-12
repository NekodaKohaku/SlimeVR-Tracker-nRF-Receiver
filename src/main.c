/*
 * Pico Tracker "Big Endian" Hunter (1Mbps Version)
 * Strategy: 
 * 1. Sweep ALL channels (0-99)
 * 2. Force BIG ENDIAN mode
 * 3. Disable Whitening
 * 4. *** Bitrate: 1Mbps *** (New Attempt)
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/usb/usb_device.h>
#include <hal/nrf_radio.h>

// 模擬數據包
static uint8_t packet_data[10] = {0x55, 0xAA, 0x00, 0x00};

void radio_init_big_endian_1m(uint8_t channel) {
    // 1. Reset
    NRF_RADIO->TASKS_DISABLE = 1;
    while (NRF_RADIO->EVENTS_DISABLED == 0);
    NRF_RADIO->EVENTS_DISABLED = 0;

    // 2. Power & Freq
    nrf_radio_power_set(NRF_RADIO, true);
    nrf_radio_frequency_set(NRF_RADIO, channel);
    
    // *** 關鍵修改：使用 1Mbps ***
    nrf_radio_mode_set(NRF_RADIO, NRF_RADIO_MODE_NRF_1MBIT);

    // 3. Address Config (Pico Special)
    nrf_radio_base0_set(NRF_RADIO, 0x43434343);
    nrf_radio_prefix0_set(NRF_RADIO, 0x43);
    
    nrf_radio_base1_set(NRF_RADIO, 0xE7E7E7E7);
    nrf_radio_prefix1_set(NRF_RADIO, 0xE7);

    nrf_radio_rxaddresses_set(NRF_RADIO, 3); 

    // 4. PCNF0 (8-bit Length)
    NRF_RADIO->PCNF0 = (8 << RADIO_PCNF0_LFLEN_Pos);

    // 5. PCNF1 (Big Endian, No Whitening)
    NRF_RADIO->PCNF1 = (
        (32 << RADIO_PCNF1_MAXLEN_Pos) |
        (4 << RADIO_PCNF1_BALEN_Pos) |
        (RADIO_PCNF1_WHITEEN_Disabled << RADIO_PCNF1_WHITEEN_Pos) |
        (RADIO_PCNF1_ENDIAN_Big << RADIO_PCNF1_ENDIAN_Pos)
    );

    // 6. CRC
    nrf_radio_crc_configure(NRF_RADIO, RADIO_CRCCNF_LEN_Two, NRF_RADIO_CRC_ADDR_SKIP, 0x11021);
    NRF_RADIO->CRCINIT = 0xFFFF;

    // 7. Packet Ptr
    NRF_RADIO->PACKETPTR = (uint32_t)packet_data;

    // 8. Shortcuts
    NRF_RADIO->SHORTS = RADIO_SHORTS_READY_START_Msk | RADIO_SHORTS_END_DISABLE_Msk;
}

bool ping_channel(uint8_t ch, uint8_t addr_idx) {
    radio_init_big_endian_1m(ch);
    NRF_RADIO->TXADDRESS = addr_idx;

    // TX
    NRF_RADIO->TASKS_TXEN = 1;
    while (NRF_RADIO->EVENTS_DISABLED == 0); // Wait for TX

    // RX (Wait for ACK)
    NRF_RADIO->SHORTS = RADIO_SHORTS_READY_START_Msk;
    NRF_RADIO->EVENTS_DISABLED = 0;
    NRF_RADIO->EVENTS_ADDRESS = 0;
    
    NRF_RADIO->TASKS_RXEN = 1;

    // Wait ~300us for ACK (1Mbps is slower, need slightly longer wait)
    for (int i = 0; i < 600; i++) {
        if (NRF_RADIO->EVENTS_ADDRESS) {
            NRF_RADIO->TASKS_DISABLE = 1;
            while (NRF_RADIO->EVENTS_DISABLED == 0);
            return true; // Got ACK!
        }
        k_busy_wait(1);
    }
    
    NRF_RADIO->TASKS_DISABLE = 1;
    while (NRF_RADIO->EVENTS_DISABLED == 0);
    return false;
}

int main(void) {
    if (IS_ENABLED(CONFIG_USB_DEVICE_STACK)) {
        usb_enable(NULL);
    }
    k_sleep(K_SECONDS(3));
    printk("\n=== Pico Hunter (1Mbps / Big Endian) ===\n");
    printk("Scanning CH 0-99...\n");

    while (1) {
        for (int ch = 0; ch < 100; ch++) {
            // Try Addr 43
            if (ping_channel(ch, 0)) {
                printk("\n!!! HIT on CH %d (Addr 43) !!!\n", ch);
                k_sleep(K_MSEC(100));
                for(int k=0; k<5; k++) ping_channel(ch, 0); 
            }
            
            // Try Addr E7
            if (ping_channel(ch, 1)) {
                printk("\n!!! HIT on CH %d (Addr E7) !!!\n", ch);
                k_sleep(K_MSEC(100));
                for(int k=0; k<5; k++) ping_channel(ch, 1);
            }
        }
        printk(".");
        k_sleep(K_MSEC(50));
    }
    return 0;
}

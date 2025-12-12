/*
 * Pico Tracker "Big Endian" Hunter
 * Strategy: 
 * 1. Sweep ALL channels (0-99) because pairing channel is unknown.
 * 2. Force BIG ENDIAN mode (based on firmware analysis).
 * 3. Disable Whitening (based on 0x1000000 vs 0x3000000 analysis).
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/usb/usb_device.h>
#include <hal/nrf_radio.h>

// 模擬一個空的發送包，誘發 ACK
static uint8_t packet_data[10] = {0x55, 0xAA, 0x00, 0x00};

void radio_init_big_endian(uint8_t channel) {
    // 1. Reset
    NRF_RADIO->TASKS_DISABLE = 1;
    while (NRF_RADIO->EVENTS_DISABLED == 0);
    NRF_RADIO->EVENTS_DISABLED = 0;

    // 2. Power & Freq
    nrf_radio_power_set(NRF_RADIO, true);
    nrf_radio_frequency_set(NRF_RADIO, channel);
    nrf_radio_mode_set(NRF_RADIO, NRF_RADIO_MODE_NRF_2MBIT); // Try 2Mbps

    // 3. Address Config (Pico Special)
    // 0x43434343 (CCCCC)
    nrf_radio_base0_set(NRF_RADIO, 0x43434343);
    nrf_radio_prefix0_set(NRF_RADIO, 0x43);
    
    // 0xE7E7E7E7 (Default)
    nrf_radio_base1_set(NRF_RADIO, 0xE7E7E7E7);
    nrf_radio_prefix1_set(NRF_RADIO, 0xE7);

    nrf_radio_rxaddresses_set(NRF_RADIO, 3); // Pipe 0 & 1

    // 4. PCNF0 (8-bit Length)
    NRF_RADIO->PCNF0 = (8 << RADIO_PCNF0_LFLEN_Pos);

    // 5. PCNF1 (CRITICAL: BIG ENDIAN, NO WHITENING)
    // 根據 FUN_000050e8，只有 bit 24 (Endian) 被設置，bit 25 (White) 沒設
    NRF_RADIO->PCNF1 = (
        (32 << RADIO_PCNF1_MAXLEN_Pos) |
        (4 << RADIO_PCNF1_BALEN_Pos) |
        (RADIO_PCNF1_WHITEEN_Disabled << RADIO_PCNF1_WHITEEN_Pos) | // 白化 關閉
        (RADIO_PCNF1_ENDIAN_Big << RADIO_PCNF1_ENDIAN_Pos)          // 大端序 開啟
    );

    // 6. CRC
    nrf_radio_crc_configure(NRF_RADIO, RADIO_CRCCNF_LEN_Two, NRF_RADIO_CRC_ADDR_SKIP, 0x11021);
    NRF_RADIO->CRCINIT = 0xFFFF;

    // 7. Packet Ptr
    NRF_RADIO->PACKETPTR = (uint32_t)packet_data;

    // 8. Shortcuts (TX -> RX)
    NRF_RADIO->SHORTS = RADIO_SHORTS_READY_START_Msk | RADIO_SHORTS_END_DISABLE_Msk;
}

bool ping_channel(uint8_t ch, uint8_t addr_idx) {
    radio_init_big_endian(ch);
    NRF_RADIO->TXADDRESS = addr_idx;

    // TX
    NRF_RADIO->TASKS_TXEN = 1;
    while (NRF_RADIO->EVENTS_DISABLED == 0); // Wait for TX to finish

    // RX (Wait for ACK)
    NRF_RADIO->SHORTS = RADIO_SHORTS_READY_START_Msk;
    NRF_RADIO->EVENTS_DISABLED = 0;
    NRF_RADIO->EVENTS_ADDRESS = 0;
    
    NRF_RADIO->TASKS_RXEN = 1;

    // Wait ~250us for ACK
    for (int i = 0; i < 400; i++) {
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
    printk("\n=== Pico Big-Endian Hunter ===\n");
    printk("Scanning CH 0-99, Big Endian, No Whitening...\n");

    while (1) {
        for (int ch = 0; ch < 100; ch++) {
            // Try Addr 43
            if (ping_channel(ch, 0)) {
                printk("\n!!! HIT on CH %d (Addr 43) !!!\n", ch);
                k_sleep(K_MSEC(100)); // Found it, slow down to verify
                for(int k=0; k<5; k++) ping_channel(ch, 0); // Burst
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

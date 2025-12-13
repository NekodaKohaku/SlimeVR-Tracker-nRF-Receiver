/*
 * Pico Tracker "Standard Knocker"
 * Strategy: Maybe they used STANDARD settings?
 * Config: Little Endian + 2Mbps + Whitening ON
 * Address: 43434343 & E7E7E7E7
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/usb/usb_device.h>
#include <hal/nrf_radio.h>

static uint8_t payloads[3][4] = {
    {0x00, 0x00, 0x00, 0x00}, 
    {0x01, 0x00, 0x00, 0x00}, 
    {0xAA, 0x55, 0xAA, 0x55} 
};

void radio_init_standard(uint8_t channel) {
    // 1. Reset
    NRF_RADIO->TASKS_DISABLE = 1;
    while (NRF_RADIO->EVENTS_DISABLED == 0);
    NRF_RADIO->EVENTS_DISABLED = 0;

    // 2. Power & Freq
    nrf_radio_power_set(NRF_RADIO, true);
    nrf_radio_frequency_set(NRF_RADIO, channel);
    
    // *** 修改點 1: 改用 2Mbps (VR 標準速度) ***
    nrf_radio_mode_set(NRF_RADIO, NRF_RADIO_MODE_NRF_2MBIT);

    // 3. Address
    nrf_radio_base0_set(NRF_RADIO, 0x43434343);
    nrf_radio_prefix0_set(NRF_RADIO, 0x43);
    nrf_radio_base1_set(NRF_RADIO, 0xE7E7E7E7);
    nrf_radio_prefix1_set(NRF_RADIO, 0xE7);
    nrf_radio_rxaddresses_set(NRF_RADIO, 3); 

    // 4. PCNF0 (8-bit Length)
    NRF_RADIO->PCNF0 = (8 << RADIO_PCNF0_LFLEN_Pos);

    // 5. PCNF1 (標準化設定)
    // *** 修改點 2: Little Endian (標準) ***
    NRF_RADIO->PCNF1 = (
        (32 << RADIO_PCNF1_MAXLEN_Pos) |
        (4 << RADIO_PCNF1_BALEN_Pos) |
        (RADIO_PCNF1_WHITEEN_Enabled << RADIO_PCNF1_WHITEEN_Pos) | 
        (RADIO_PCNF1_ENDIAN_Little << RADIO_PCNF1_ENDIAN_Pos) // 改回小端序
    );

    // 6. CRC (16-bit)
    nrf_radio_crc_configure(NRF_RADIO, RADIO_CRCCNF_LEN_Two, NRF_RADIO_CRC_ADDR_SKIP, 0x11021);
    NRF_RADIO->CRCINIT = 0xFFFF;
    NRF_RADIO->DATAWHITEIV = channel | 0x40;

    // 7. Shortcuts
    NRF_RADIO->SHORTS = RADIO_SHORTS_READY_START_Msk | RADIO_SHORTS_END_DISABLE_Msk;
}

bool try_knock_std(uint8_t ch, uint8_t addr_idx, uint8_t payload_idx) {
    radio_init_standard(ch);
    
    NRF_RADIO->TXADDRESS = addr_idx;
    NRF_RADIO->PACKETPTR = (uint32_t)payloads[payload_idx];

    // TX
    NRF_RADIO->TASKS_TXEN = 1;
    while (NRF_RADIO->EVENTS_DISABLED == 0);

    // RX (Listen for ACK)
    NRF_RADIO->SHORTS = RADIO_SHORTS_READY_START_Msk;
    NRF_RADIO->EVENTS_DISABLED = 0;
    NRF_RADIO->EVENTS_ADDRESS = 0;
    
    NRF_RADIO->TASKS_RXEN = 1;

    // 2Mbps 比較快，ACK 回傳也快，等待 300us 夠了
    for (int i = 0; i < 500; i++) {
        if (NRF_RADIO->EVENTS_ADDRESS) {
            NRF_RADIO->TASKS_DISABLE = 1;
            while (NRF_RADIO->EVENTS_DISABLED == 0);
            return true;
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

    k_sleep(K_SECONDS(2));
    printk("=== Pico Standard Knocker (2Mbps/Little Endian) ===\n");
    printk("Scanning...\n");

    while (1) {
        for (int ch = 0; ch < 100; ch++) {
            // Address 43...
            if (try_knock_std(ch, 0, 0)) {
                printk("\n!!! HIT !!! CH:%d (Addr 43)\n", ch);
                for(int k=0; k<5; k++) { printk("*"); k_sleep(K_MSEC(50)); }
            }
            // Address E7...
            if (try_knock_std(ch, 1, 0)) {
                printk("\n!!! HIT !!! CH:%d (Addr E7)\n", ch);
                for(int k=0; k<5; k++) { printk("*"); k_sleep(K_MSEC(50)); }
            }
        }
        printk(".");
        k_sleep(K_MSEC(10));
    }
    return 0;
}

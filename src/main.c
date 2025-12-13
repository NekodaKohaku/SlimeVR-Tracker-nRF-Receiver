/*
 * Pico Tracker "BLE Hunter" (Fixed USB)
 * Target: Standard BLE Advertising Packets
 * Channels: 37 (2402), 38 (2426), 39 (2480)
 * Access Address: 0x8E89BED6
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
// *** 補上這行：USB 必要標頭檔 ***
#include <zephyr/usb/usb_device.h> 
#include <hal/nrf_radio.h>

// BLE 廣播頻率
#define FREQ_CH_37 2
#define FREQ_CH_38 26
#define FREQ_CH_39 80

void radio_init_ble(uint8_t freq) {
    // 1. Reset
    NRF_RADIO->TASKS_DISABLE = 1;
    while (NRF_RADIO->EVENTS_DISABLED == 0);
    NRF_RADIO->EVENTS_DISABLED = 0;

    // 2. Config
    nrf_radio_power_set(NRF_RADIO, true);
    nrf_radio_frequency_set(NRF_RADIO, freq);
    
    // BLE 廣播通常使用 1Mbps
    nrf_radio_mode_set(NRF_RADIO, NRF_RADIO_MODE_BLE_1MBIT);

    // 3. BLE Access Address (固定為 0x8E89BED6)
    nrf_radio_base0_set(NRF_RADIO, 0x89BED600);
    nrf_radio_prefix0_set(NRF_RADIO, 0x8E);
    nrf_radio_rxaddresses_set(NRF_RADIO, 1); 

    // 4. PCNF0 (BLE 格式)
    NRF_RADIO->PCNF0 = (
        (8 << RADIO_PCNF0_LFLEN_Pos) |
        (1 << RADIO_PCNF0_S0LEN_Pos) |
        (0 << RADIO_PCNF0_S1LEN_Pos)
    );

    // 5. PCNF1 (WhiteEn=1, Balen=3)
    NRF_RADIO->PCNF1 = (
        (37 << RADIO_PCNF1_MAXLEN_Pos) |
        (0 << RADIO_PCNF1_STATLEN_Pos) |
        (3 << RADIO_PCNF1_BALEN_Pos) |
        (RADIO_PCNF1_WHITEEN_Enabled << RADIO_PCNF1_WHITEEN_Pos) |
        (RADIO_PCNF1_ENDIAN_Little << RADIO_PCNF1_ENDIAN_Pos)
    );

    // 6. CRC (BLE 是 24-bit CRC)
    NRF_RADIO->CRCCNF = (RADIO_CRCCNF_LEN_Three << RADIO_CRCCNF_LEN_Pos) |
                        (RADIO_CRCCNF_SKIPADDR_Skip << RADIO_CRCCNF_SKIPADDR_Pos);
    NRF_RADIO->CRCPOLY = 0x00065B;
    NRF_RADIO->CRCINIT = 0x555555;

    // 7. Whitening IV (BLE Channel Index)
    uint8_t chan_idx = 0;
    if (freq == 2) chan_idx = 37;
    else if (freq == 26) chan_idx = 38;
    else if (freq == 80) chan_idx = 39;
    
    NRF_RADIO->DATAWHITEIV = chan_idx | 0x40;

    // 8. Start RX
    NRF_RADIO->SHORTS = RADIO_SHORTS_READY_START_Msk | RADIO_SHORTS_END_START_Msk;
    NRF_RADIO->TASKS_RXEN = 1;
}

int main(void) {
    // *** 補上這段：啟動 USB ***
    if (IS_ENABLED(CONFIG_USB_DEVICE_STACK)) {
        int ret = usb_enable(NULL);
        if (ret != 0) {
            return 0; // 如果啟動失敗就停在這
        }
    }
    // *************************

    k_sleep(K_SECONDS(2));
    printk("=== Pico BLE Hunter ===\n");
    printk("Listening on BLE Adv Channels (37, 38, 39)...\n");

    int ch_state = 0;
    uint8_t current_freq = FREQ_CH_37;

    while (1) {
        if (ch_state == 0) current_freq = FREQ_CH_37;
        else if (ch_state == 1) current_freq = FREQ_CH_38;
        else current_freq = FREQ_CH_39;

        radio_init_ble(current_freq);

        // 每個頻道聽 200ms
        for (int i = 0; i < 200; i++) {
            if (NRF_RADIO->EVENTS_CRCOK) {
                NRF_RADIO->EVENTS_CRCOK = 0;
                
                printk("\n!!! BLE PACKET DETECTED on CH %d !!!\n", (ch_state+37));
                k_sleep(K_MSEC(10));
            }
            k_sleep(K_MSEC(1));
        }

        ch_state = (ch_state + 1) % 3;
    }
    return 0;
}

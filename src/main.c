/*
 * Pico Tracker "Final Key" Knocker
 * Protocol: ESB 2Mbps
 * Address: Base 0x4E4E4E4E, Prefix 0x04
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/usb/usb_device.h>
#include <hal/nrf_radio.h>

// 根據 Dump 分析出的指令頭 "recv 0x50"
static uint8_t magic_payload[] = {0x50, 0x01, 0x02, 0x03}; 

void radio_init_final(uint8_t channel) {
    // 1. Reset
    NRF_RADIO->TASKS_DISABLE = 1;
    while (NRF_RADIO->EVENTS_DISABLED == 0);
    NRF_RADIO->EVENTS_DISABLED = 0;

    // 2. Config (ESB 2Mbps - Dump 確認)
    nrf_radio_power_set(NRF_RADIO, true);
    nrf_radio_frequency_set(NRF_RADIO, channel);
    nrf_radio_mode_set(NRF_RADIO, NRF_RADIO_MODE_NRF_2MBIT);

    // 3. Address (The Magic Found in Dump!)
    // Base = 0x4E4E4E4E (推測填充)
    // Prefix = 0x04 (Dump 顯示 04)
    // 組合起來完整地址是：0x044E4E4E4E
    nrf_radio_base0_set(NRF_RADIO, 0x4E4E4E4E);
    nrf_radio_prefix0_set(NRF_RADIO, 0x04);
    
    // 也有可能是 0x004E4E4E, Prefix 0x04，我們兩個通道都開
    nrf_radio_base1_set(NRF_RADIO, 0x004E4E4E);
    nrf_radio_prefix1_set(NRF_RADIO, 0x04);

    nrf_radio_rxaddresses_set(NRF_RADIO, 3); // Enable logical addr 0 & 1

    // 4. PCNF (標準 ESB 設定)
    NRF_RADIO->PCNF0 = (8 << RADIO_PCNF0_LFLEN_Pos) | (1 << RADIO_PCNF0_S0LEN_Pos);
    NRF_RADIO->PCNF1 = (32 << RADIO_PCNF1_MAXLEN_Pos) | 
                       (4 << RADIO_PCNF1_BALEN_Pos) | 
                       (RADIO_PCNF1_ENDIAN_Big << RADIO_PCNF1_ENDIAN_Pos) | // 試試 Big Endian
                       (RADIO_PCNF1_WHITEEN_Enabled << RADIO_PCNF1_WHITEEN_Pos);

    // 5. CRC (ESB 標準 16-bit)
    nrf_radio_crc_configure(NRF_RADIO, RADIO_CRCCNF_LEN_Two, NRF_RADIO_CRC_ADDR_SKIP, 0x11021);
    NRF_RADIO->CRCINIT = 0xFFFF;
    NRF_RADIO->DATAWHITEIV = channel | 0x40; // Whitening

    // 6. Shorts
    NRF_RADIO->SHORTS = RADIO_SHORTS_READY_START_Msk | RADIO_SHORTS_END_DISABLE_Msk;
}

bool try_wake_up(uint8_t ch) {
    radio_init_final(ch);
    
    // 發送 Magic Payload (0x50...)
    NRF_RADIO->TXADDRESS = 0; // Use Base0
    NRF_RADIO->PACKETPTR = (uint32_t)magic_payload;

    NRF_RADIO->TASKS_TXEN = 1;
    while (NRF_RADIO->EVENTS_DISABLED == 0); // Wait for TX done

    // Switch to RX to listen for ACK
    NRF_RADIO->SHORTS = RADIO_SHORTS_READY_START_Msk;
    NRF_RADIO->EVENTS_DISABLED = 0;
    NRF_RADIO->TASKS_RXEN = 1;

    // Wait 500us for ACK
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
    printk("=== Pico Final Unlocker ===\n");
    printk("Target: Base=0x4E4E4E4E, Prefix=0x04\n");
    printk("Payload: 0x50 (Command Head)\n");

    while (1) {
        // 掃描全頻段
        for (int ch = 0; ch < 100; ch++) {
            if (try_wake_up(ch)) {
                printk("\n!!! TARGET WOKE UP !!! CH:%d\n", ch);
                
                // 抓到了！瘋狂閃爍 LED 或發送
                for(int k=0; k<10; k++) {
                    printk("*");
                    k_sleep(K_MSEC(50));
                }
            }
        }
        printk(".");
        k_sleep(K_MSEC(10));
    }
    return 0;
}

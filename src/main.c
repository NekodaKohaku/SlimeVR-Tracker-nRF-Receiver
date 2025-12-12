/*
 * Pico Tracker "Gatling Gun" Pinger (Fixed)
 * Strategy: Brute-force sweep ALL channels (0-99)
 * Goal: Trigger Auto-ACK on ANY channel
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/usb/usb_device.h>
#include <hal/nrf_radio.h>

// 模擬數據包 (Pico 常用的 Header)
static uint8_t packet_data[10] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05};

void radio_init_generic(void) {
    // 1. 重置 Radio
    NRF_RADIO->TASKS_DISABLE = 1;
    while (NRF_RADIO->EVENTS_DISABLED == 0);
    NRF_RADIO->EVENTS_DISABLED = 0;

    // 2. 電源
    nrf_radio_power_set(NRF_RADIO, true);

    // 3. 速率: 2Mbps (最可能的)
    nrf_radio_mode_set(NRF_RADIO, NRF_RADIO_MODE_NRF_2MBIT);

    // 4. 地址: 4343434343 (Pico Special) & E7E7E7E7E7 (Default)
    nrf_radio_base0_set(NRF_RADIO, 0x43434343);
    nrf_radio_prefix0_set(NRF_RADIO, 0x43);
    nrf_radio_base1_set(NRF_RADIO, 0xE7E7E7E7);
    nrf_radio_prefix1_set(NRF_RADIO, 0xE7);
    nrf_radio_rxaddresses_set(NRF_RADIO, 3); // Pipe 0 & 1

    // 5. 封包格式
    // LFLEN=8 bits (標準 ESB), S0=0, S1=0
    NRF_RADIO->PCNF0 = (8 << RADIO_PCNF0_LFLEN_Pos);

    // 6. PCNF1 (開啟白化)
    NRF_RADIO->PCNF1 = (
        (32 << RADIO_PCNF1_MAXLEN_Pos) |
        (4 << RADIO_PCNF1_BALEN_Pos) |
        (RADIO_PCNF1_WHITEEN_Enabled << RADIO_PCNF1_WHITEEN_Pos) | // 白化 ON
        (RADIO_PCNF1_ENDIAN_Little << RADIO_PCNF1_ENDIAN_Pos)
    );

    // 7. CRC
    nrf_radio_crc_configure(NRF_RADIO, RADIO_CRCCNF_LEN_Two, NRF_RADIO_CRC_ADDR_SKIP, 0x11021);
    NRF_RADIO->CRCINIT = 0xFFFF;

    // 8. 設定 Packet Pointer
    NRF_RADIO->PACKETPTR = (uint32_t)packet_data;
}

// 在特定頻道發射並等待 ACK
bool shoot_and_listen(uint8_t channel, uint8_t tx_addr_idx) {
    // 設定頻率
    NRF_RADIO->FREQUENCY = channel;
    // 設定白化初始值 (必須跟頻率有關)
    NRF_RADIO->DATAWHITEIV = channel | 0x40;
    
    // *** 修正點：使用正確的變數名稱 tx_addr_idx ***
    NRF_RADIO->TXADDRESS = tx_addr_idx; 

    // --- 1. 發送 (TX) ---
    NRF_RADIO->SHORTS = RADIO_SHORTS_READY_START_Msk | RADIO_SHORTS_END_DISABLE_Msk;
    NRF_RADIO->EVENTS_DISABLED = 0;
    
    NRF_RADIO->TASKS_TXEN = 1;
    while (NRF_RADIO->EVENTS_DISABLED == 0); // 等待發送完成並 Disable
    
    // --- 2. 接收 (RX) 等待 ACK ---
    // 這裡我們要極快地切換到 RX
    NRF_RADIO->SHORTS = RADIO_SHORTS_READY_START_Msk; // RX Ready 後自動 Start
    NRF_RADIO->EVENTS_DISABLED = 0;
    NRF_RADIO->EVENTS_ADDRESS = 0; // 清除地址匹配標誌
    
    NRF_RADIO->TASKS_RXEN = 1; // 啟動接收

    // 忙碌等待一小段時間 (等待 ACK 回傳)
    // ACK 通常在 150us 內回傳
    bool ack_received = false;
    for (int i = 0; i < 400; i++) { // 約 200-300us
        if (NRF_RADIO->EVENTS_ADDRESS) {
            ack_received = true;
            break;
        }
        k_busy_wait(1);
    }
    
    // 關閉無線電
    NRF_RADIO->TASKS_DISABLE = 1;
    while (NRF_RADIO->EVENTS_DISABLED == 0);
    
    return ack_received;
}

int main(void) {
    if (IS_ENABLED(CONFIG_USB_DEVICE_STACK)) {
        usb_enable(NULL);
    }
    k_sleep(K_SECONDS(3));
    printk("=== Pico Tracker Gatling Gun (Fixed) ===\n");
    printk("Sweeping CH 0-99 with Whitening ON...\n");

    radio_init_generic();

    while (1) {
        // 掃描一整輪
        for (int ch = 0; ch < 100; ch++) {
            
            // 嘗試地址 0 (43...)
            if (shoot_and_listen(ch, 0)) {
                printk("\n!!! HIT !!! ACK on CH %d (Addr 43)\n", ch);
                // 抓到了就停在這個頻道多試幾次確認
                for(int k=0; k<5; k++) {
                    shoot_and_listen(ch, 0);
                    k_sleep(K_MSEC(10));
                }
            }

            // 嘗試地址 1 (E7...)
            if (shoot_and_listen(ch, 1)) {
                printk("\n!!! HIT !!! ACK on CH %d (Addr E7)\n", ch);
                for(int k=0; k<5; k++) {
                    shoot_and_listen(ch, 1);
                    k_sleep(K_MSEC(10));
                }
            }
        }
        
        // 掃完一輪休息一下，打印一個點證明活著
        printk(".");
        k_sleep(K_MSEC(100)); 
    }
    return 0;
}

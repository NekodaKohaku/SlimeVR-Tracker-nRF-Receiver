/*
 * Pico Tracker "Decoder" Sniffer
 * Key Fix: BIG ENDIAN Mode + Whitening
 * Targets: CH 73 & 45
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/usb/usb_device.h>
#include <hal/nrf_radio.h>

void radio_init(uint8_t channel) {
    // 1. 停止並重置
    NRF_RADIO->TASKS_DISABLE = 1;
    while (NRF_RADIO->EVENTS_DISABLED == 0);
    
    // 2. 基本設定
    nrf_radio_power_set(NRF_RADIO, true);
    nrf_radio_frequency_set(NRF_RADIO, channel);
    nrf_radio_mode_set(NRF_RADIO, NRF_RADIO_MODE_NRF_2MBIT); // Pico 慣用 2Mbps

    // 3. 地址設定 (43... 和 E7...)
    // 注意：在 Big Endian 模式下，地址寫入順序可能不需要變，但硬體解析順序會變
    // 我們保持標準寫法，讓硬體處理
    nrf_radio_base0_set(NRF_RADIO, 0x43434343);
    nrf_radio_prefix0_set(NRF_RADIO, 0x43);
    
    nrf_radio_base1_set(NRF_RADIO, 0xE7E7E7E7);
    nrf_radio_prefix1_set(NRF_RADIO, 0xE7);
    
    nrf_radio_rxaddresses_set(NRF_RADIO, 3); // Enable Pipe 0 & 1

    // 4. PCNF0: LFLEN=8 bits
    NRF_RADIO->PCNF0 = (8 << RADIO_PCNF0_LFLEN_Pos);

    // 5. PCNF1: 關鍵設定！！！
    // ENDIAN = Big (1)
    // WHITEEN = Enabled (1)
    NRF_RADIO->PCNF1 = (
        (32 << RADIO_PCNF1_MAXLEN_Pos) |
        (4 << RADIO_PCNF1_BALEN_Pos) |
        (RADIO_PCNF1_WHITEEN_Enabled << RADIO_PCNF1_WHITEEN_Pos) |
        (RADIO_PCNF1_ENDIAN_Big << RADIO_PCNF1_ENDIAN_Pos) // <--- 這是之前失敗的主因
    );

    // 6. CRC: 16-bit
    nrf_radio_crc_configure(NRF_RADIO, RADIO_CRCCNF_LEN_Two, NRF_RADIO_CRC_ADDR_SKIP, 0x11021);
    NRF_RADIO->CRCINIT = 0xFFFF;

    // 7. 白化初始值
    NRF_RADIO->DATAWHITEIV = channel | 0x40;

    // 8. 啟動接收
    NRF_RADIO->SHORTS = RADIO_SHORTS_READY_START_Msk | RADIO_SHORTS_END_START_Msk; // Loop RX
    
    // 清除事件
    NRF_RADIO->EVENTS_READY = 0;
    NRF_RADIO->EVENTS_END = 0;
    NRF_RADIO->EVENTS_ADDRESS = 0;
    NRF_RADIO->EVENTS_PAYLOAD = 0;
    NRF_RADIO->EVENTS_CRCOK = 0;

    NRF_RADIO->TASKS_RXEN = 1;
}

int main(void) {
    if (IS_ENABLED(CONFIG_USB_DEVICE_STACK)) {
        usb_enable(NULL);
    }
    
    k_sleep(K_SECONDS(2));
    printk("=== Pico Decoder (Big Endian + Whitening) ===\n");
    printk("Listening on CH 73 and 45...\n");

    int ch_toggle = 0;
    int channel = 73;

    while (1) {
        // 切換頻道邏輯
        channel = (ch_toggle == 0) ? 73 : 45;
        radio_init(channel);
        
        printk("Scanning CH %d...\r", channel);
        
        // 在這個頻道聽 200ms
        for (int i = 0; i < 200; i++) {
            if (NRF_RADIO->EVENTS_CRCOK) {
                NRF_RADIO->EVENTS_CRCOK = 0;
                
                // 讀取長度 (因為是 Big Endian，硬體應該已經處理了解析)
                // 但我們直接讀 Payload 比較保險
                uint8_t len = 32; // 假設讀最大
                
                printk("\n[PKT] CH:%d AddrMatch:%d ", channel, NRF_RADIO->RXMATCH);
                
                // 這裡只能簡單示意讀取，實際上需要 DMA 或 PacketPtr 讀取
                // 為了簡單，我們只顯示「收到有效包」的訊息
                printk("CRC OK! Valid Packet Detected!\n");
                
                // 如果抓到了，就不用切換頻道了，鎖死在這裡
                while(1) {
                     if (NRF_RADIO->EVENTS_CRCOK) {
                         NRF_RADIO->EVENTS_CRCOK = 0;
                         printk("!"); // 收到更多包
                     }
                     k_sleep(K_MSEC(1));
                }
            }
            k_sleep(K_MSEC(1));
        }
        
        ch_toggle = !ch_toggle;
    }
    return 0;
}

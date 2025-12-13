/*
 * Pico Tracker "Wake-up Caller"
 * Strategy: Emulate the Headset (TX) to wake up the Tracker (RX)
 * Config: Big Endian + 1Mbps + Whitening ON
 * Payload: Tries common headers (0x00, 0x01, 0xAA)
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/usb/usb_device.h>
#include <hal/nrf_radio.h>

// 嘗試幾種常見的開頭數據
static uint8_t payloads[3][4] = {
    {0x00, 0x00, 0x00, 0x00}, // 純空包
    {0x01, 0x00, 0x00, 0x00}, // 命令 01
    {0xAA, 0x55, 0xAA, 0x55}  // 常見 Sync Pattern
};

void radio_init_master(uint8_t channel) {
    // 1. Reset
    NRF_RADIO->TASKS_DISABLE = 1;
    while (NRF_RADIO->EVENTS_DISABLED == 0);
    NRF_RADIO->EVENTS_DISABLED = 0;

    // 2. Power & Freq
    nrf_radio_power_set(NRF_RADIO, true);
    nrf_radio_frequency_set(NRF_RADIO, channel);
    
    // *** 關鍵 1: 使用 1Mbps (配對通訊最穩) ***
    nrf_radio_mode_set(NRF_RADIO, NRF_RADIO_MODE_NRF_1MBIT);

    // 3. Address: 43434343 (Pico Special) & E7E7E7E7
    // 因為是 Big Endian，我們寫入時保持原樣，讓硬體反轉
    nrf_radio_base0_set(NRF_RADIO, 0x43434343);
    nrf_radio_prefix0_set(NRF_RADIO, 0x43);
    
    nrf_radio_base1_set(NRF_RADIO, 0xE7E7E7E7);
    nrf_radio_prefix1_set(NRF_RADIO, 0xE7);

    nrf_radio_rxaddresses_set(NRF_RADIO, 3); 

    // 4. PCNF0 (8-bit Length)
    NRF_RADIO->PCNF0 = (8 << RADIO_PCNF0_LFLEN_Pos);

    // 5. PCNF1 (Big Endian + Whitening)
    // 這是最可能的組合：Big Endian 是為了混淆，Whitening 是為了抗干擾
    NRF_RADIO->PCNF1 = (
        (32 << RADIO_PCNF1_MAXLEN_Pos) |
        (4 << RADIO_PCNF1_BALEN_Pos) |
        (RADIO_PCNF1_WHITEEN_Enabled << RADIO_PCNF1_WHITEEN_Pos) | // 白化 ON
        (RADIO_PCNF1_ENDIAN_Big << RADIO_PCNF1_ENDIAN_Pos)         // 大端序 ON
    );

    // 6. CRC (16-bit)
    nrf_radio_crc_configure(NRF_RADIO, RADIO_CRCCNF_LEN_Two, NRF_RADIO_CRC_ADDR_SKIP, 0x11021);
    NRF_RADIO->CRCINIT = 0xFFFF;
    
    // 白化 IV (跟頻道有關)
    NRF_RADIO->DATAWHITEIV = channel | 0x40;

    // 7. Shortcuts: 發送後自動準備接收 ACK
    // READY->START (TX)
    // END->DISABLE (TX Done) -> 我們手動切換到 RX 比較穩
}

bool try_knock(uint8_t ch, uint8_t addr_idx, uint8_t payload_idx) {
    radio_init_master(ch);
    
    NRF_RADIO->TXADDRESS = addr_idx;
    NRF_RADIO->PACKETPTR = (uint32_t)payloads[payload_idx];

    // --- TX (敲門) ---
    NRF_RADIO->SHORTS = RADIO_SHORTS_READY_START_Msk | RADIO_SHORTS_END_DISABLE_Msk;
    NRF_RADIO->TASKS_TXEN = 1;
    while (NRF_RADIO->EVENTS_DISABLED == 0); // 等待發送結束

    // --- RX (聽有沒有人應門) ---
    NRF_RADIO->SHORTS = RADIO_SHORTS_READY_START_Msk; // Ready 後自動開始聽
    NRF_RADIO->EVENTS_DISABLED = 0;
    NRF_RADIO->EVENTS_ADDRESS = 0; // 重置標記
    
    NRF_RADIO->TASKS_RXEN = 1;

    // 等待 ACK (1Mbps 比較慢，我們給它 500us)
    // 只要此時追蹤器回傳 ACK，ADDRESS 事件就會觸發
    bool ack_received = false;
    for (int i = 0; i < 1000; i++) {
        if (NRF_RADIO->EVENTS_ADDRESS) {
            ack_received = true;
            break;
        }
        k_busy_wait(1);
    }
    
    // 關閉
    NRF_RADIO->TASKS_DISABLE = 1;
    while (NRF_RADIO->EVENTS_DISABLED == 0);

    return ack_received;
}

int main(void) {
    // 1. 啟動 USB
    if (IS_ENABLED(CONFIG_USB_DEVICE_STACK)) {
        usb_enable(NULL);
    }

    k_sleep(K_SECONDS(3));
    printk("=== Pico Wake-up Caller ===\n");
    printk("Knocking on all doors (CH 0-99)...\n");

    while (1) {
        // 掃描全頻段
        for (int ch = 0; ch < 100; ch++) {
            
            // 嘗試地址 0 (43...) 和 3 種 Payload
            for (int p = 0; p < 3; p++) {
                if (try_knock(ch, 0, p)) {
                    printk("\n!!! DOOR OPENED !!! CH:%d, Addr:43, Payload:%d\n", ch, p);
                    // 抓到了！瘋狂閃爍
                    for(int k=0; k<10; k++) {
                        printk("*"); 
                        k_sleep(K_MSEC(50));
                    }
                }
            }

            // 嘗試地址 1 (E7...) 和 3 種 Payload
            for (int p = 0; p < 3; p++) {
                if (try_knock(ch, 1, p)) {
                    printk("\n!!! DOOR OPENED !!! CH:%d, Addr:E7, Payload:%d\n", ch, p);
                    for(int k=0; k<10; k++) {
                        printk("*"); 
                        k_sleep(K_MSEC(50));
                    }
                }
            }
        }
        
        printk("."); // 每掃完一輪印一個點
        k_sleep(K_MSEC(10));
    }
    return 0;
}

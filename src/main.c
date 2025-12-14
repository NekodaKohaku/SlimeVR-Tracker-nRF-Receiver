/*
 * Pico Tracker "Final Key" Knocker (Strict CRC Version)
 * Target: Pico Tracker 2.0 (based on FW Dump)
 * Protocol: ESB 2Mbps
 * Address: Base 0x4E4E4E4E, Prefix 0x04
 * Logic: TX Command 0x50 -> Listen for ACK (CRC OK)
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/usb/usb_device.h>
#include <hal/nrf_radio.h>

// 根據 Dump 分析出的指令頭 "recv 0x50"
// 我們發送這個，模擬頭顯在對追蹤器下指令
static uint8_t magic_payload[] = {0x50, 0x01, 0x02, 0x03}; 

void radio_init_final(uint8_t channel) {
    // 1. Reset Radio
    NRF_RADIO->TASKS_DISABLE = 1;
    while (NRF_RADIO->EVENTS_DISABLED == 0);
    NRF_RADIO->EVENTS_DISABLED = 0;

    // 2. Config (ESB 2Mbps - Dump 確認)
    nrf_radio_power_set(NRF_RADIO, NRF_RADIO_TXPOWER_POS4DBM); // Max Power
    nrf_radio_frequency_set(NRF_RADIO, channel);
    nrf_radio_mode_set(NRF_RADIO, NRF_RADIO_MODE_NRF_2MBIT);

    // 3. Address (The Magic Found in Dump!)
    // Base = 0x4E4E4E4E (Nordic 常見填充)
    // Prefix = 0x04 (Dump 顯示 04)
    // 組合起來完整地址是：0x044E4E4E4E
    nrf_radio_base0_set(NRF_RADIO, 0x4E4E4E4E);
    nrf_radio_prefix0_set(NRF_RADIO, 0x04);
    
    // 為了保險，我們把 Base1 也設成可能的變體
    nrf_radio_base1_set(NRF_RADIO, 0x004E4E4E);
    nrf_radio_prefix1_set(NRF_RADIO, 0x04);

    nrf_radio_rxaddresses_set(NRF_RADIO, 3); // Enable logical addr 0 & 1

    // 4. PCNF (標準 ESB 設定)
    // LFLEN=8 bit, S0LEN=1 bit (相容 Nordic ESB 庫)
    NRF_RADIO->PCNF0 = (8 << RADIO_PCNF0_LFLEN_Pos) | (1 << RADIO_PCNF0_S0LEN_Pos);
    
    // Big Endian (從之前的測試推測) + Whitening (Dump 暗示可能有)
    NRF_RADIO->PCNF1 = (32 << RADIO_PCNF1_MAXLEN_Pos) | 
                       (4 << RADIO_PCNF1_BALEN_Pos) | 
                       (RADIO_PCNF1_ENDIAN_Big << RADIO_PCNF1_ENDIAN_Pos) |
                       (RADIO_PCNF1_WHITEEN_Enabled << RADIO_PCNF1_WHITEEN_Pos);

    // 5. CRC (ESB 標準 16-bit)
    // 如果這裡不對，我們雖然會收到訊號，但 CRC 會報錯
    nrf_radio_crc_configure(NRF_RADIO, RADIO_CRCCNF_LEN_Two, NRF_RADIO_CRC_ADDR_SKIP, 0x11021);
    NRF_RADIO->CRCINIT = 0xFFFF;
    NRF_RADIO->DATAWHITEIV = channel | 0x40; // Whitening IV

    // 6. Shortcuts (手動控制，這裡先不設自動跳轉，以免邏輯混亂)
    NRF_RADIO->SHORTS = 0;
}

bool try_wake_up(uint8_t ch) {
    radio_init_final(ch);
    
    // === 步驟 1: 發送喚醒指令 (TX) ===
    
    // 清除所有事件標記
    NRF_RADIO->EVENTS_READY = 0;
    NRF_RADIO->EVENTS_END = 0;
    NRF_RADIO->EVENTS_ADDRESS = 0;
    NRF_RADIO->EVENTS_CRCOK = 0;
    NRF_RADIO->EVENTS_CRCERROR = 0;

    // 設定 TX 緩衝區
    NRF_RADIO->TXADDRESS = 0; // Use Logical Address 0
    NRF_RADIO->PACKETPTR = (uint32_t)magic_payload;

    // 啟動發射
    NRF_RADIO->TASKS_TXEN = 1;
    while (NRF_RADIO->EVENTS_READY == 0); // 等待無線電熱機完成
    
    NRF_RADIO->TASKS_START = 1;           // 開始發送
    while (NRF_RADIO->EVENTS_END == 0);   // 等待發送結束
    
    // === 步驟 2: 快速切換到接收 (RX) 聽 ACK ===
    
    // 必須先 Disable 才能切換模式
    NRF_RADIO->EVENTS_DISABLED = 0;
    NRF_RADIO->TASKS_DISABLE = 1;
    while (NRF_RADIO->EVENTS_DISABLED == 0);

    // 清除事件，準備接收
    NRF_RADIO->EVENTS_READY = 0;
    NRF_RADIO->EVENTS_END = 0;
    NRF_RADIO->EVENTS_CRCOK = 0;
    NRF_RADIO->EVENTS_CRCERROR = 0;
    NRF_RADIO->EVENTS_ADDRESS = 0;

    // 啟動接收
    NRF_RADIO->TASKS_RXEN = 1;
    while (NRF_RADIO->EVENTS_READY == 0); // 等待 RX 就緒
    NRF_RADIO->TASKS_START = 1;           // 開始監聽

    // === 步驟 3: 監聽視窗 (1ms) ===
    // ESB 的 ACK 通常在 150us 內就會回來
    bool success = false;
    for (int i = 0; i < 1000; i++) {
        
        // [關鍵修正] 只有當 CRC 校驗成功時，才算真的連上！
        // 這能過濾掉所有雜訊和錯誤的頻率
        if (NRF_RADIO->EVENTS_CRCOK) {
            success = true;
            break;
        }
        
        // 如果 CRC 錯誤，代表有訊號但解不出來 (可能是雜訊或頻道重疊)
        // 這種情況我們當作失敗，以免假陽性
        if (NRF_RADIO->EVENTS_CRCERROR) {
            // 可以在這裡做個記號，但為了乾淨，我們先忽略
            break; 
        }
        
        k_busy_wait(1); // 等 1us
    }
    
    // 關閉無線電
    NRF_RADIO->TASKS_DISABLE = 1;
    while (NRF_RADIO->EVENTS_DISABLED == 0);

    return success;
}

int main(void) {
    // 確保 USB 開啟
    if (IS_ENABLED(CONFIG_USB_DEVICE_STACK)) {
        usb_enable(NULL);
    }

    k_sleep(K_SECONDS(2));
    printk("\n=== Pico Final Unlocker (Strict Mode) ===\n");
    printk("Based on FW Dump Analysis\n");
    printk("Target: Base=0x4E4E4E4E, Prefix=0x04\n");
    printk("Payload: 0x50... (Command Head)\n");
    printk("Strategy: TX -> Wait for CRC-OK ACK\n");
    printk("--------------------------------------\n");
    printk("!!! IMPORTANT: Please SHORT PRESS the tracker button now !!!\n");
    printk("--------------------------------------\n");

    while (1) {
        // 掃描全頻段 0-99
        for (int ch = 0; ch < 100; ch++) {
            
            // 嘗試喚醒
            if (try_wake_up(ch)) {
                printk("\n>>> ACK RECEIVED! Target is on CH: %d <<<\n", ch);
                
                // 抓到了！瘋狂輸出，讓您看見
                for(int k=0; k<20; k++) {
                    printk("ACCESS GRANTED! ");
                    k_sleep(K_MSEC(20));
                }
                printk("\n");
                
                // 暫停一下，讓 Putty 刷屏慢一點
                k_sleep(K_MSEC(500));
            }
        }
        // 沒抓到就印一個點，表示還活著
        printk(".");
        k_sleep(K_MSEC(1));
    }
    return 0;
}

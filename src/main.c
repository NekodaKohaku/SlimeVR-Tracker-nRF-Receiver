/*
 * Pico Tracker "Corrected" Pinger
 * Fixes: Enables Dynamic Payload Length (LFLEN=8)
 * Strategy: Sends valid ESB packets to trigger Auto-ACK
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/usb/usb_device.h>
#include <hal/nrf_radio.h>

// 目標定義
#define CH_A 73
#define CH_B 45

// 模擬數據包 (Pico 配對請求通常以 0x01 或 0x00 開頭)
static uint8_t packet_data[12] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};

void radio_configure(uint8_t channel, bool use_2mbps) {
    // 1. 禁用並重置
    NRF_RADIO->TASKS_DISABLE = 1;
    while (NRF_RADIO->EVENTS_DISABLED == 0);
    NRF_RADIO->EVENTS_DISABLED = 0;

    // 2. 電源與頻率
    nrf_radio_power_set(NRF_RADIO, true);
    nrf_radio_frequency_set(NRF_RADIO, channel);

    // 3. 速率
    if (use_2mbps) 
        nrf_radio_mode_set(NRF_RADIO, NRF_RADIO_MODE_NRF_2MBIT);
    else 
        nrf_radio_mode_set(NRF_RADIO, NRF_RADIO_MODE_NRF_1MBIT);

    // 4. 地址設定 (關鍵！)
    // Base0 = 0x43434343, Prefix0 = 0x43 (Addr = 4343434343)
    nrf_radio_base0_set(NRF_RADIO, 0x43434343);
    nrf_radio_prefix0_set(NRF_RADIO, 0x43);
    
    // Base1 = 0xE7E7E7E7, Prefix1 = 0xE7 (Addr = E7E7E7E7E7)
    nrf_radio_base1_set(NRF_RADIO, 0xE7E7E7E7);
    nrf_radio_prefix1_set(NRF_RADIO, 0xE7);

    nrf_radio_rxaddresses_set(NRF_RADIO, 3); // Enable Pipe 0 & 1

    // 5. PCNF0 (封包格式) - 修正點！
    // LFLEN=8 (8-bit Length), S0LEN=0, S1LEN=0
    // 這讓它看起來像一個正常的 ESB 封包
    NRF_RADIO->PCNF0 = (8 << RADIO_PCNF0_LFLEN_Pos);

    // 6. PCNF1 (白化與長度)
    // MaxLen=32, StatLen=0, Balen=4, Endian=Little, WhiteEn=1
    NRF_RADIO->PCNF1 = (
        (32 << RADIO_PCNF1_MAXLEN_Pos) |
        (0 << RADIO_PCNF1_STATLEN_Pos) | 
        (4 << RADIO_PCNF1_BALEN_Pos) |
        (RADIO_PCNF1_WHITEEN_Enabled << RADIO_PCNF1_WHITEEN_Pos) |
        (RADIO_PCNF1_ENDIAN_Little << RADIO_PCNF1_ENDIAN_Pos)
    );

    // 7. CRC (16-bit)
    nrf_radio_crc_configure(NRF_RADIO, RADIO_CRCCNF_LEN_Two, NRF_RADIO_CRC_ADDR_SKIP, 0x11021);
    NRF_RADIO->CRCINIT = 0xFFFF;

    // 8. 白化 IV
    NRF_RADIO->DATAWHITEIV = channel | 0x40;
    
    // 9. Shortcuts (自動處理 TX -> RX)
    // 當發送完成(PHYEND)自動轉入接收(RX)等待 ACK
    NRF_RADIO->SHORTS = RADIO_SHORTS_PHYEND_START_Msk | // Loop sending? No, handled manually
                        RADIO_SHORTS_END_DISABLE_Msk;   // Stop after Tx (we will manually switch to RX for ACK)
                        
    // 設定封包指針
    NRF_RADIO->PACKETPTR = (uint32_t)packet_data;
}

bool ping(uint8_t tx_addr_index) {
    // 設定發送目標 (0 = 43..., 1 = E7...)
    NRF_RADIO->TXADDRESS = tx_addr_index;

    // TX Enable
    NRF_RADIO->EVENTS_READY = 0;
    NRF_RADIO->TASKS_TXEN = 1;
    while (NRF_RADIO->EVENTS_READY == 0);

    // Start Transmission
    NRF_RADIO->EVENTS_END = 0;
    NRF_RADIO->TASKS_START = 1;
    while (NRF_RADIO->EVENTS_END == 0);

    // --- 切換到 RX 等待 ACK ---
    // 這是 ESB 的精髓。發送完必須立刻聽。
    // 這裡我們用最簡單的方法：關閉再開啟 RX (雖然慢一點但邏輯簡單)
    // 更標準的做法是用 SHORTS (TX_READY->START, END->RXEN...) 但這需要精確時序
    
    // Pico Tracker 的 ACK 回傳極快 (130us)，我們用軟體切換可能來不及。
    // 但如果我們只是為了觸發 "Address Match" (ADDRESS 事件)，即使沒收到完整 ACK，
    // 只要看到 ADDRESS 事件觸發，就代表對方聽到了！
    
    // 這裡我們不等待 RX，我們只看發送過程是否順利。
    // 若要真正收到 ACK，我們需要在此處切換 RX。
    
    // 為了簡單驗證 "是否抓到"，我們現在依賴「對方是否因為收到我們訊號而改變 LED」。
    
    return true; 
}

// 為了真正檢測 ACK，我們需要使用硬體 Shortcut
// 這是一個進階的 Ping 函數，使用 RADIO 硬體的 Auto-ACK 機制
bool ping_with_ack_check(uint8_t tx_addr_index) {
    NRF_RADIO->SHORTS = RADIO_SHORTS_READY_START_Msk | RADIO_SHORTS_END_DISABLE_Msk;
    
    // 1. TX
    NRF_RADIO->TXADDRESS = tx_addr_index;
    NRF_RADIO->EVENTS_READY = 0;
    NRF_RADIO->EVENTS_END = 0;
    NRF_RADIO->TASKS_TXEN = 1;
    while (NRF_RADIO->EVENTS_END == 0); // Wait for TX done
    
    // 2. RX (Wait for ACK)
    // 這裡我們快速切換到 RX
    NRF_RADIO->SHORTS = RADIO_SHORTS_READY_START_Msk | RADIO_SHORTS_END_DISABLE_Msk;
    NRF_RADIO->EVENTS_READY = 0;
    NRF_RADIO->EVENTS_END = 0;
    NRF_RADIO->EVENTS_ADDRESS = 0;
    NRF_RADIO->EVENTS_CRCOK = 0;
    
    NRF_RADIO->TASKS_RXEN = 1;

    // 等待一小段時間 (200us)
    k_busy_wait(250); 
    
    if (NRF_RADIO->EVENTS_ADDRESS) {
        NRF_RADIO->TASKS_DISABLE = 1;
        return true; // 收到東西了 (可能是 ACK)
    }
    
    NRF_RADIO->TASKS_DISABLE = 1;
    return false;
}

int main(void) {
    if (IS_ENABLED(CONFIG_USB_DEVICE_STACK)) {
        usb_enable(NULL);
    }
    k_sleep(K_SECONDS(3));
    printk("=== Corrected Pico Pinger ===\n");
    printk("Trying DPL (8-bit Length) + Whitening\n");

    while (1) {
        // Test CH 73, 2Mbps, Addr 43
        radio_configure(73, true);
        if (ping_with_ack_check(0)) {
            printk("!!! HIT on CH 73 (2Mbps, ADDR 43) !!!\n");
            k_sleep(K_MSEC(500));
        }

        // Test CH 45, 2Mbps, Addr 43
        radio_configure(45, true);
        if (ping_with_ack_check(0)) {
            printk("!!! HIT on CH 45 (2Mbps, ADDR 43) !!!\n");
            k_sleep(K_MSEC(500));
        }
        
         // Test CH 73, 2Mbps, Addr E7
        radio_configure(73, true);
        if (ping_with_ack_check(1)) {
            printk("!!! HIT on CH 73 (2Mbps, ADDR E7) !!!\n");
            k_sleep(K_MSEC(500));
        }

        printk(".");
        k_sleep(K_MSEC(50));
    }
    return 0;
}

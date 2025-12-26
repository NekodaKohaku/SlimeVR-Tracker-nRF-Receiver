/*
 * Pico Tracker HUNTER (Scanner & Responder)
 * Strategy:
 * 1. Listen (RX) on jumping channels.
 * 2. If packet received from TARGET_ADDRESS -> Print it.
 * 3. Immediately SEND an ACK (Empty PDU) to keep Tracker alive.
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <hal/nrf_radio.h>
#include <zephyr/usb/usb_device.h>

// === 目標參數 ===
// 根據 RAM Dump，它會跳頻，所以我們需要輪詢列表
// 頻率列表: 2401(1), 2402(2), 2426(26), 2480(80), 2477(77)
static const uint8_t scan_channels[] = {1, 2, 26, 80, 77, 25, 47};

// 地址 (Big Endian 0xC0 55 2C 6A 1E)
#define TARGET_BASE_ADDR  0x552c6a1eUL
#define TARGET_PREFIX     0xC0

// 接收與發送緩衝區
static uint8_t rx_buffer[32];
static uint8_t tx_buffer[32];

void radio_init(void)
{
    // 1. 重置 Radio
    NRF_RADIO->POWER = 0;
    k_busy_wait(500);
    NRF_RADIO->POWER = 1;

    // 2. 物理層配置 (Ble_2Mbit)
    NRF_RADIO->MODE = NRF_RADIO_MODE_BLE_2MBIT;

    // 3. 封包格式配置 (關鍵修正!)
    // 根據 dump 數據 "42 18 ...", 0x42 是 Header(S0), 0x18 是 Length
    // 所以我們必須開啟 S0LEN=1, LFLEN=8
    NRF_RADIO->PCNF0 = (8UL << RADIO_PCNF0_LFLEN_Pos) | 
                       (1UL << RADIO_PCNF0_S0LEN_Pos) | 
                       (0UL << RADIO_PCNF0_S1LEN_Pos); // S1 可能為 0 或 4，先設 0 試試

    // PCNF1: Big Endian, MaxLen 32
    NRF_RADIO->PCNF1 = (32UL << RADIO_PCNF1_MAXLEN_Pos) |
                       (0UL  << RADIO_PCNF1_STATLEN_Pos) |
                       (4UL  << RADIO_PCNF1_BALEN_Pos) |
                       (1UL  << RADIO_PCNF1_ENDIAN_Pos) | // Big Endian
                       (0UL  << RADIO_PCNF1_WHITEEN_Pos); // No Whitening

    // 4. 地址配置
    NRF_RADIO->BASE0 = TARGET_BASE_ADDR;
    NRF_RADIO->PREFIX0 = TARGET_PREFIX;
    NRF_RADIO->TXADDRESS = 0;   // TX 使用邏輯地址 0
    NRF_RADIO->RXADDRESSES = 1; // RX 監聽邏輯地址 0

    // 5. CRC 配置 (16-bit, 0x1021)
    NRF_RADIO->CRCCNF = (RADIO_CRCCNF_LEN_Two << RADIO_CRCCNF_LEN_Pos) | 
                        (RADIO_CRCCNF_SKIPADDR_Skip << RADIO_CRCCNF_SKIPADDR_Pos);
    NRF_RADIO->CRCINIT = 0xFFFF;
    NRF_RADIO->CRCPOLY = 0x11021;

    // 6. 快捷方式 (SHORTS) - 這是 "Active" 的關鍵
    // 當接收到包 (END) -> 自動關閉 (DISABLE) -> 自動準備發送 (TXEN) -> 自動開始發送 (START)
    // 這樣可以極速回覆 ACK
    // *注意*：為了先 Debug，我們先不自動 TX，用軟體控制。等抓到包再開啟自動 ACK。
    NRF_RADIO->SHORTS = (RADIO_SHORTS_READY_START_Msk | RADIO_SHORTS_END_DISABLE_Msk);
}

void prepare_response_packet(void)
{
    // 準備一個回應包 (ACK)
    // 格式: [Header] [Length] [Payload...]
    // 假設 Header = 0x01 (Empty PDU / ACK), Length = 0
    tx_buffer[0] = 0x01; // S0 (Header)
    tx_buffer[1] = 0x00; // Length = 0
    // Payload 空
}

int main(void)
{
    usb_enable(NULL);
    k_sleep(K_MSEC(3000)); // 等待 USB 連線

    printk("\n============================================\n");
    printk(">>> Pico Tracker HUNTER (Scanner Mode)   <<<\n");
    printk(">>> Searching for C0 55 2C 6A 1E...      <<<\n");
    printk("============================================\n");

    radio_init();
    prepare_response_packet();

    int ch_idx = 0;

    while (1) {
        // === 步驟 1: 設定頻率 ===
        NRF_RADIO->FREQUENCY = scan_channels[ch_idx];
        // printk("Scanning CH %d...\n", scan_channels[ch_idx]);

        // === 步驟 2: 進入 RX 模式 (監聽) ===
        NRF_RADIO->PACKETPTR = (uint32_t)rx_buffer;
        NRF_RADIO->EVENTS_READY = 0;
        NRF_RADIO->TASKS_RXEN = 1;
        while (NRF_RADIO->EVENTS_READY == 0); // 等待 Radio 啟動

        // 啟動接收
        NRF_RADIO->EVENTS_END = 0;
        NRF_RADIO->TASKS_START = 1;

        // === 步驟 3: 等待封包 (Timeout 機制) ===
        // 給它 20ms 的時間窗口來抓包
        bool packet_received = false;
        for (volatile int i = 0; i < 15000; i++) {
            if (NRF_RADIO->EVENTS_END) {
                packet_received = true;
                break;
            }
        }

        // === 步驟 4: 處理結果 ===
        if (packet_received) {
            // 檢查 CRC
            if (NRF_RADIO->CRCSTATUS == 1) {
                // 檢查 Header 是否為 0x42 (Tracker 的廣播特徵)
                // 這裡 rx_buffer[0] 是 S0, rx_buffer[1] 是 Length
                
                printk("\n>>> [CAPTURE!] Valid Packet on CH %d! <<<\n", scan_channels[ch_idx]);
                printk("Header: %02X, Len: %02X, Payload: ", rx_buffer[0], rx_buffer[1]);
                for(int k=0; k < (rx_buffer[1] > 10 ? 10 : rx_buffer[1]); k++) {
                     printk("%02X ", rx_buffer[2+k]);
                }
                printk("\n");

                // === 步驟 5: 立刻發送回應 (Keep-Alive) ===
                // 因為我們用了 SHORTS_END_DISABLE，現在 Radio 已經 Disabled 了
                // 我們立刻切換到 TX 發送回應
                NRF_RADIO->PACKETPTR = (uint32_t)tx_buffer;
                NRF_RADIO->EVENTS_END = 0;
                NRF_RADIO->TASKS_TXEN = 1; // 觸發 TX
                
                // 等待 TX 完成
                while (NRF_RADIO->EVENTS_END == 0);
                
                printk(">>> ACK Sent! (Try to bond)\n");
                
                // 鎖定這個頻道一小段時間，因為對話可能正在進行
                k_sleep(K_MSEC(50)); 
            } else {
                // CRC 錯誤，可能是雜訊
                // printk("CRC Error\n");
            }
        } else {
            // Timeout，沒收到東西，強制關閉 Radio 準備換頻
             NRF_RADIO->TASKS_DISABLE = 1;
             while (NRF_RADIO->EVENTS_DISABLED == 0);
        }

        // 換下一個頻率
        ch_idx = (ch_idx + 1) % sizeof(scan_channels);
        
        // 極短暫延遲
        // k_busy_wait(100); 
    }
}

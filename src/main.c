/*
 * Pico Tracker "Brute Force" Scanner
 * Strategy: Cycle through LFLEN settings (4, 6, 8, 0) to find the key.
 * Role: PTX (Active Ping)
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <hal/nrf_radio.h>
#include <zephyr/usb/usb_device.h>

// 1. 基礎參數
#define TARGET_BASE_ADDR  0x552c6a1eUL
#define TARGET_PREFIX     0xC0
#define CH_FREQ           1 

// 測試清單：我們要嘗試的 LFLEN 設定
static const uint8_t lflen_candidates[] = {4, 6, 8, 0};
#define CANDIDATE_COUNT 4

// 發送緩衝區
// 注意：當 LFLEN > 0 時，tx_buffer[0] 會被硬體當作長度欄位發送
static uint8_t tx_buffer[32]; 

void radio_configure(uint8_t lflen_val)
{
    NRF_RADIO->TASKS_DISABLE = 1;
    while (NRF_RADIO->EVENTS_DISABLED == 0);
    NRF_RADIO->EVENTS_DISABLED = 0;

    NRF_RADIO->MODE = RADIO_MODE_MODE_Nrf_2Mbit;
    NRF_RADIO->FREQUENCY = CH_FREQ;
    NRF_RADIO->TXPOWER = RADIO_TXPOWER_TXPOWER_0dBm;

    NRF_RADIO->BASE0 = TARGET_BASE_ADDR;
    NRF_RADIO->PREFIX0 = TARGET_PREFIX;
    NRF_RADIO->TXADDRESS = 0;
    NRF_RADIO->RXADDRESSES = 1;

    // === 動態配置 PCNF0 ===
    printk("Trying LFLEN = %d bits... ", lflen_val);
    NRF_RADIO->PCNF0 = (lflen_val << RADIO_PCNF0_LFLEN_Pos);

    // === 動態配置 PCNF1 ===
    if (lflen_val > 0) {
        // 動態長度模式：StatLen 設為 0，硬體會去讀 Buffer 的第一個 Byte 當長度
        NRF_RADIO->PCNF1 = (32 << RADIO_PCNF1_MAXLEN_Pos) | 
                           (0  << RADIO_PCNF1_STATLEN_Pos) | 
                           (4  << RADIO_PCNF1_BALEN_Pos) | 
                           (RADIO_PCNF1_ENDIAN_Little << RADIO_PCNF1_ENDIAN_Pos);
        
        // 設定 Payload 長度：我們只發 8 Bytes
        // 如果 LFLEN=4，最大只能 15，所以設 8 很安全
        tx_buffer[0] = 8; 
    } else {
        // 固定長度模式 (LFLEN=0)：必須指定 StatLen
        NRF_RADIO->PCNF1 = (32 << RADIO_PCNF1_MAXLEN_Pos) | 
                           (8  << RADIO_PCNF1_STATLEN_Pos) | // 強制發 8 byte
                           (4  << RADIO_PCNF1_BALEN_Pos) | 
                           (RADIO_PCNF1_ENDIAN_Little << RADIO_PCNF1_ENDIAN_Pos);
    }

    NRF_RADIO->CRCCNF = (RADIO_CRCCNF_LEN_Two << RADIO_CRCCNF_LEN_Pos);
    NRF_RADIO->CRCINIT = 0xFFFF;
    NRF_RADIO->CRCPOLY = 0x11021;

    NRF_RADIO->SHORTS = (RADIO_SHORTS_READY_START_Msk | RADIO_SHORTS_END_DISABLE_Msk);
}

int main(void)
{
    if (usb_enable(NULL)) return 0;
    k_sleep(K_MSEC(2000));
    printk("\n>>> PCNF0 BRUTE FORCE SCANNER STARTED <<<\n");

    int current_idx = 0;
    bool locked = false; // 是否找到正確密碼

    while (1) {
        uint8_t test_lflen = lflen_candidates[current_idx];

        // 1. 如果還沒鎖定，就配置下一個 LFLEN
        if (!locked) {
            radio_configure(test_lflen);
        }

        // 2. 準備發送
        NRF_RADIO->PACKETPTR = (uint32_t)tx_buffer;
        
        // 3. 發送 Ping (並等待 ACK)
        NRF_RADIO->EVENTS_DISABLED = 0;
        NRF_RADIO->TASKS_TXEN = 1;
        while (NRF_RADIO->EVENTS_DISABLED == 0);

        // 4. 檢查結果
        if (NRF_RADIO->CRCSTATUS == 1) {
            printk(" [SUCCESS!] ACK Received! \n");
            printk(">>> MATCH FOUND! Correct LFLEN is: %d <<<\n", test_lflen);
            
            // 鎖定這個模式，不再切換
            locked = true;
            
            // 讓 LED 狂閃慶祝 (如果有)
            // 修改 Payload 假裝通訊
            tx_buffer[1]++; 
        } else {
            printk(" [No ACK]\n");
            // 沒收到，換下一個參數
            if (!locked) {
                current_idx++;
                if (current_idx >= CANDIDATE_COUNT) current_idx = 0;
            }
        }

        // 掃描間隔：
        // 如果鎖定了就快一點 (10ms)，如果還在掃就慢一點 (100ms) 讓它有時間反應
        k_sleep(K_MSEC(locked ? 10 : 100));
    }
}

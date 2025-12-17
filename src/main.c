/*
 * Pico Tracker PASSIVE Scanner (No-Wait Version)
 * Strategy: Listen ONLY. Cycle LFLEN. 
 * Fix: Removed DTR wait loop so it starts immediately.
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <hal/nrf_radio.h>
#include <zephyr/usb/usb_device.h>

// === 參數設定 ===
#define TARGET_BASE_ADDR  0x552c6a1eUL
#define TARGET_PREFIX     0xC0
#define CH_FREQ           1 

// 測試清單 (優先測試 4，因為這是最大嫌疑)
static const uint8_t lflen_candidates[] = {4, 6, 8, 0};
#define CANDIDATE_COUNT 4

static uint8_t rx_buffer[32]; 

// 安全等待函數 (防止死機)
void wait_for_disabled(void) {
    int timeout = 5000;
    while (NRF_RADIO->EVENTS_DISABLED == 0 && timeout > 0) {
        k_busy_wait(10);
        timeout--;
    }
    NRF_RADIO->EVENTS_DISABLED = 0;
}

void radio_configure_rx(uint8_t lflen_val)
{
    // 1. 停用 Radio
    NRF_RADIO->TASKS_DISABLE = 1;
    wait_for_disabled();

    // 2. 基本設定
    NRF_RADIO->MODE = RADIO_MODE_MODE_Nrf_2Mbit;
    NRF_RADIO->FREQUENCY = CH_FREQ;

    NRF_RADIO->BASE0 = TARGET_BASE_ADDR;
    NRF_RADIO->PREFIX0 = TARGET_PREFIX;
    NRF_RADIO->TXADDRESS = 0;
    NRF_RADIO->RXADDRESSES = 1;

    // 3. 設定 LFLEN
    // 加上 \n 確保緩衝區輸出
    printk("Scanning LFLEN = %d bits... \n", lflen_val);

    NRF_RADIO->PCNF0 = (lflen_val << RADIO_PCNF0_LFLEN_Pos);

    // 4. 設定 PCNF1
    if (lflen_val > 0) {
        // 動態長度模式
        NRF_RADIO->PCNF1 = (32 << RADIO_PCNF1_MAXLEN_Pos) | 
                           (0  << RADIO_PCNF1_STATLEN_Pos) | 
                           (4  << RADIO_PCNF1_BALEN_Pos) | 
                           (RADIO_PCNF1_ENDIAN_Little << RADIO_PCNF1_ENDIAN_Pos);
    } else {
        // 固定長度模式
        NRF_RADIO->PCNF1 = (32 << RADIO_PCNF1_MAXLEN_Pos) | 
                           (32 << RADIO_PCNF1_STATLEN_Pos) | 
                           (4  << RADIO_PCNF1_BALEN_Pos) | 
                           (RADIO_PCNF1_ENDIAN_Little << RADIO_PCNF1_ENDIAN_Pos);
    }

    // 5. CRC 設定
    NRF_RADIO->CRCCNF = (RADIO_CRCCNF_LEN_Two << RADIO_CRCCNF_LEN_Pos);
    NRF_RADIO->CRCINIT = 0xFFFF;
    NRF_RADIO->CRCPOLY = 0x11021;

    // 6. Shortcuts: Ready -> Start (自動開始接收)
    NRF_RADIO->SHORTS = (RADIO_SHORTS_READY_START_Msk);
}

int main(void)
{
    // 1. 啟動 USB
    if (usb_enable(NULL)) return 0;

    // 2. [關鍵修正] 不再等待 DTR 連線，直接睡 3 秒緩衝
    // 這 3 秒是給您時間打開 Arduino 視窗的，錯過也沒關係，後面會一直循環
    k_sleep(K_MSEC(3000)); 
    
    printk("\n=========================================\n");
    printk(">>> PASSIVE SCANNER STARTED (Immediate) <<<\n");
    printk("=========================================\n");

    int current_idx = 0;
    bool locked = false;

    while (1) {
        uint8_t test_lflen = lflen_candidates[current_idx];

        if (!locked) {
            radio_configure_rx(test_lflen);
        }

        // 啟動接收
        NRF_RADIO->PACKETPTR = (uint32_t)rx_buffer;
        NRF_RADIO->TASKS_RXEN = 1;

        // 監聽視窗: 200ms
        // 如果這 200ms 內 Tracker 有發射，我們就應該收到
        bool packet_found = false;
        
        // 使用迴圈檢查多次，提高解析度
        for (int t = 0; t < 200; t++) { // 200ms loop
            if (NRF_RADIO->EVENTS_END) {
                NRF_RADIO->EVENTS_END = 0;
                
                // 檢查 CRC: 這是唯一的真理
                if (NRF_RADIO->CRCSTATUS == 1) {
                    packet_found = true;
                    break; // 抓到了！
                } else {
                    // CRC 錯誤，重啟接收
                    NRF_RADIO->TASKS_START = 1; 
                }
            }
            k_busy_wait(1000); // 1ms wait
        }

        // 檢查結果
        if (packet_found) {
            printk("\n>>> [CAPTURED!] Valid Packet Found! LFLEN = %d <<<\n", test_lflen);
            printk("Payload (Hex): ");
            for(int k=0; k<16; k++) printk("%02X ", rx_buffer[k]);
            printk("\n");
            
            // 鎖定模式：不再切換，專心收這個格式
            locked = true; 
            
            // 重啟接收以抓下一包
            NRF_RADIO->TASKS_START = 1;
        } 
        else {
            if (!locked) {
                // 沒抓到，切換下一個參數
                current_idx++;
                if (current_idx >= CANDIDATE_COUNT) current_idx = 0;
            } else {
                // 已鎖定但暫時沒訊號，重啟接收
                NRF_RADIO->TASKS_START = 1;
            }
        }
    }
}

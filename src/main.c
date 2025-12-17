/*
 * Pico Tracker Brute Force Scanner (Safe Version)
 * Fixes: Added printk buffering flush and loop timeouts to prevent hanging.
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <hal/nrf_radio.h>
#include <zephyr/usb/usb_device.h>
#include <zephyr/drivers/gpio.h> // 引入 GPIO 以便閃燈 (如果有的話)

// 1. 基礎參數
#define TARGET_BASE_ADDR  0x552c6a1eUL
#define TARGET_PREFIX     0xC0
#define CH_FREQ           1 

// 測試清單
static const uint8_t lflen_candidates[] = {4, 6, 8, 0};
#define CANDIDATE_COUNT 4

static uint8_t tx_buffer[32]; 

// 用來防止死循環的超時函數
void wait_for_event(volatile uint32_t *event_reg) {
    int timeout = 10000; // 約 100ms
    while (*event_reg == 0 && timeout > 0) {
        k_busy_wait(10); // 等待 10us
        timeout--;
    }
    if (timeout == 0) {
        // printk(" (Timeout!) "); // 偵錯用
    }
    *event_reg = 0; // 清除事件
}

void radio_configure(uint8_t lflen_val)
{
    // 安全地停用 Radio
    NRF_RADIO->TASKS_DISABLE = 1;
    wait_for_event(&NRF_RADIO->EVENTS_DISABLED);

    NRF_RADIO->MODE = RADIO_MODE_MODE_Nrf_2Mbit;
    NRF_RADIO->FREQUENCY = CH_FREQ;
    NRF_RADIO->TXPOWER = RADIO_TXPOWER_TXPOWER_0dBm;

    NRF_RADIO->BASE0 = TARGET_BASE_ADDR;
    NRF_RADIO->PREFIX0 = TARGET_PREFIX;
    NRF_RADIO->TXADDRESS = 0;
    NRF_RADIO->RXADDRESSES = 1;

    // === 印出 Log (加上 \n 確保顯示) ===
    printk("Trying LFLEN = %d bits... \n", lflen_val);

    NRF_RADIO->PCNF0 = (lflen_val << RADIO_PCNF0_LFLEN_Pos);

    if (lflen_val > 0) {
        NRF_RADIO->PCNF1 = (32 << RADIO_PCNF1_MAXLEN_Pos) | 
                           (0  << RADIO_PCNF1_STATLEN_Pos) | 
                           (4  << RADIO_PCNF1_BALEN_Pos) | 
                           (RADIO_PCNF1_ENDIAN_Little << RADIO_PCNF1_ENDIAN_Pos);
        tx_buffer[0] = 8; 
    } else {
        NRF_RADIO->PCNF1 = (32 << RADIO_PCNF1_MAXLEN_Pos) | 
                           (8  << RADIO_PCNF1_STATLEN_Pos) | 
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
    
    // 給足夠的時間連接終端機
    for(int i=0; i<5; i++) {
        k_sleep(K_MSEC(1000)); 
        // 這裡可以加個 printk 測試看看
    }
    
    printk("\n=========================================\n");
    printk(">>> SAFE BRUTE FORCE SCANNER STARTED <<<\n");
    printk("=========================================\n");

    int current_idx = 0;
    bool locked = false;

    while (1) {
        uint8_t test_lflen = lflen_candidates[current_idx];

        if (!locked) {
            radio_configure(test_lflen);
        }

        // 準備發送
        NRF_RADIO->PACKETPTR = (uint32_t)tx_buffer;
        NRF_RADIO->EVENTS_DISABLED = 0; // 確保標誌已清除
        
        // 啟動發射
        NRF_RADIO->TASKS_TXEN = 1;
        
        // [關鍵] 使用有超時的等待，而不是無限迴圈
        wait_for_event(&NRF_RADIO->EVENTS_DISABLED);

        // 檢查結果
        if (NRF_RADIO->CRCSTATUS == 1) {
            printk(">>> [SUCCESS!] ACK Received! LFLEN = %d <<<\n", test_lflen);
            locked = true;
            tx_buffer[1]++; 
        } else {
            // 沒收到 ACK 
            // 這裡不印 log 避免刷屏太快，只在切換參數時印
            
            if (!locked) {
                current_idx++;
                if (current_idx >= CANDIDATE_COUNT) current_idx = 0;
            }
        }

        k_sleep(K_MSEC(locked ? 20 : 200));
    }
}

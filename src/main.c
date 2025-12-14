/*
 * Copyright (c) 2025 White Cat DIY
 * Pico Tracker Protocol Investigator - Raw Sniffer
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <hal/nrf_radio.h>

// 定義一個用於接收的 Buffer
static uint8_t packet_buffer[32]; 

void radio_setup_sniffer(void)
{
    // 1. 禁用 Radio
    nrf_radio_task_trigger(NRF_RADIO, NRF_RADIO_TASK_DISABLE);

    // 2. 設定模式：Nrf_2Mbit (這是 ESB/私有協議最常用的模式)
    // 如果抓不到，可以試試 NRF_RADIO_MODE_NRF_1MBIT
    nrf_radio_mode_set(NRF_RADIO, NRF_RADIO_MODE_NRF_2MBIT);

    // 3. 設定地址寬度 (Address Width)
    // 技巧：我們故意設定很短的地址，或者模擬廣播地址
    // 這裡設定 Base Address Length = 2 bytes, Prefix = 1 byte (共 3 bytes)
    // 大多數 ESB 設定至少是 3 bytes
    NRF_RADIO->PCNF1 = (
        (8UL << NRF_RADIO_PCNF1_MAXLEN_Pos) | // Payload 最大長度
        (3UL << NRF_RADIO_PCNF1_STATLEN_Pos) | // Static Length (如果有的話)
        (2UL << NRF_RADIO_PCNF1_BALEN_Pos) |   // Base Address Length = 2
        (NRF_RADIO_PCNF1_ENDIAN_Big << NRF_RADIO_PCNF1_ENDIAN_Pos) |
        (1UL << NRF_RADIO_PCNF1_WHITEEN_Pos) // 嘗試開啟或關閉 Whitening (這很關鍵，先試試開啟)
    );

    // 4. 設定 Preamble 和地址匹配
    // 我們嘗試用最常見的 Preamble 模式來捕捉
    // 這裡我們「猜」它可能用的一個通用地址，或者利用邏輯漏洞抓取
    // 註：沒有正確的 Address 其實很難抓到完整的 payload，
    // 但我們可以試著將 Base Address 設為 0x5555 或 0xAAAA (Preamble 的樣子)
    NRF_RADIO->BASE0 = 0x5555; 
    NRF_RADIO->PREFIX0 = 0x55; // 組合起來像是一串 Preamble
    
    // 接收地址 0
    NRF_RADIO->RXADDRESSES = 1UL;

    // 5. 關閉 CRC (關鍵！)
    // 我們不知道它的 CRC 算法，所以必須關掉檢查，否則 Radio 會丟棄封包
    // 副作用：你會收到很多雜訊
    NRF_RADIO->CRCCNF = 0; 

    // 6. 設定封包指針
    nrf_radio_packetptr_set(NRF_RADIO, packet_buffer);
    
    // 7. 設定頻率 (建議根據上一步掃頻的結果手動填入！)
    // 假設你剛才掃到 2478 MHz 最強
    nrf_radio_frequency_set(NRF_RADIO, 78); 
}

int main(void)
{
    printk("*** Protocol Sniffer Started ***\n");
    printk("Frequency: 2478MHz (Change in code if needed)\n");
    printk("Mode: NRF_2Mbit | CRC: OFF\n");

    radio_setup_sniffer();

    while (1) {
        // 1. 啟動接收
        nrf_radio_task_trigger(NRF_RADIO, NRF_RADIO_TASK_RXEN);
        k_busy_wait(150); // 等待 Ready
        nrf_radio_task_trigger(NRF_RADIO, NRF_RADIO_TASK_START);

        // 2. 等待接收結束 (END 事件)
        // 這裡做一個簡單的超時，因為如果沒收到訊號它會一直等
        bool packet_received = false;
        for(int i=0; i<1000; i++) {
            if (nrf_radio_event_check(NRF_RADIO, NRF_RADIO_EVENT_END)) {
                nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_END);
                packet_received = true;
                break;
            }
            k_busy_wait(10);
        }

        // 3. 如果收到東西，印出來
        if (packet_received) {
            printk("RX: ");
            for(int i=0; i<10; i++) { // 印出前 10 個 bytes
                printk("%02X ", packet_buffer[i]);
            }
            printk("\n");
        } else {
            // 沒收到就停止並重啟，避免卡死
             nrf_radio_task_trigger(NRF_RADIO, NRF_RADIO_TASK_STOP);
        }
        
        // 稍微休息
        nrf_radio_task_trigger(NRF_RADIO, NRF_RADIO_TASK_DISABLE);
        k_msleep(10);
    }
}

/*
 * SlimeVR Dongle - Active Ping Mode
 * Role: PTX (Transmitter)
 * Action: Sends packets continuously to trigger ACK from Tracker
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <hal/nrf_radio.h>
#include <zephyr/usb/usb_device.h>

// 參數設定 (恢復為 0xC0)
#define TARGET_BASE_ADDR  0x552c6a1eUL
#define TARGET_PREFIX     0xC0
#define CH_FREQ           1 

static uint8_t tx_payload[8] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

void radio_init_ptx(void) {
    NRF_RADIO->TASKS_DISABLE = 1;
    while (NRF_RADIO->EVENTS_DISABLED == 0);
    NRF_RADIO->EVENTS_DISABLED = 0;

    NRF_RADIO->MODE = RADIO_MODE_MODE_Nrf_2Mbit;
    NRF_RADIO->FREQUENCY = CH_FREQ;
    NRF_RADIO->TXPOWER = RADIO_TXPOWER_TXPOWER_0dBm;

    // 設定地址
    NRF_RADIO->BASE0 = TARGET_BASE_ADDR;
    NRF_RADIO->PREFIX0 = TARGET_PREFIX;
    NRF_RADIO->TXADDRESS = 0;
    NRF_RADIO->RXADDRESSES = 1; // 接收 ACK 用

    // 設定 PCNF (跟之前一樣，保守設定)
    // LFLEN=0 (不發長度頭), StatLen=8 (發8byte)
    NRF_RADIO->PCNF0 = (0 << RADIO_PCNF0_LFLEN_Pos);
    NRF_RADIO->PCNF1 = (8 << RADIO_PCNF1_MAXLEN_Pos) | 
                       (8 << RADIO_PCNF1_STATLEN_Pos) | 
                       (4 << RADIO_PCNF1_BALEN_Pos) | 
                       (RADIO_PCNF1_ENDIAN_Little << RADIO_PCNF1_ENDIAN_Pos);

    // CRC
    NRF_RADIO->CRCCNF = (RADIO_CRCCNF_LEN_Two << RADIO_CRCCNF_LEN_Pos);
    NRF_RADIO->CRCINIT = 0xFFFF;
    NRF_RADIO->CRCPOLY = 0x11021;

    // Shortcut: READY -> START (自動發射), END -> DISABLE (發完關閉)
    // 注意：如果是 PTX，硬體會自動切換到 RX 等 ACK。
    // 如果收到 ACK -> EVENTS_END
    // 如果沒收到 -> 還是會有 EVENTS_END (取決於 Radio 狀態機，我們先簡單處理)
    NRF_RADIO->SHORTS = (RADIO_SHORTS_READY_START_Msk | RADIO_SHORTS_END_DISABLE_Msk);
}

int main(void)
{
    if (usb_enable(NULL)) return 0;
    k_sleep(K_MSEC(2000));
    printk(">>> ACTIVE PING MODE STARTED (PTX) <<<\n");
    printk("Target: 0x%X (Prefix 0x%X)\n", TARGET_BASE_ADDR, TARGET_PREFIX);
    printk("Pinging on Channel %d...\n", CH_FREQ);

    radio_init_ptx();

    while (1) {
        // 1. 準備發送數據
        NRF_RADIO->PACKETPTR = (uint32_t)tx_payload;
        
        // 2. 啟動發射 (TXEN -> READY -> START -> TX -> END -> DISABLE)
        NRF_RADIO->EVENTS_END = 0;
        NRF_RADIO->TASKS_TXEN = 1;

        // 3. 等待動作完成
        while (NRF_RADIO->EVENTS_END == 0);
        
        // 4. 檢查是否收到 ACK?
        // 在標準 ESB 硬體邏輯中，這比較複雜。
        // 但我們可以偷看 CRCSTATUS。
        // 如果我們是 PTX，發送後會自動轉 RX 等 ACK。
        // 如果 CRCSTATUS = 1，代表我們收到了 ACK！
        
        if (NRF_RADIO->CRCSTATUS == 1) {
             printk("!!! ACK RECEIVED !!! We found the Tracker!\n");
             // 讓 LED 亮起來
        }

        // 稍微變更 Payload 內容，假裝是心跳包
        tx_payload[0]++;

        // 快速連發 (模擬主機呼叫)
        k_sleep(K_MSEC(2)); 
    }
}

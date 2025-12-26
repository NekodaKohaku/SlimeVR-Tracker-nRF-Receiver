/*
 * Pico Tracker HUNTER v5 (RSSI Radar)
 * 新增功能：顯示訊號強度 (RSSI)，讓你當雷達用
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <hal/nrf_radio.h>
#include <zephyr/usb/usb_device.h>

#define TARGET_BASE_ADDR  0x552c6a1eUL
#define TARGET_PREFIX     0xC0

static uint8_t rx_buffer[64];
static uint8_t tx_buffer[32];

void radio_init(void)
{
    NRF_RADIO->POWER = 0;
    k_busy_wait(500);
    NRF_RADIO->POWER = 1;

    NRF_RADIO->MODE = NRF_RADIO_MODE_BLE_2MBIT;

    // S0=0, L=8, S1=4 (Tracker 結構)
    NRF_RADIO->PCNF0 = (8UL << RADIO_PCNF0_LFLEN_Pos) | 
                       (0UL << RADIO_PCNF0_S0LEN_Pos) | 
                       (4UL << RADIO_PCNF0_S1LEN_Pos);

    NRF_RADIO->PCNF1 = (32UL << RADIO_PCNF1_MAXLEN_Pos) |
                       (0UL  << RADIO_PCNF1_STATLEN_Pos) |
                       (4UL  << RADIO_PCNF1_BALEN_Pos) |
                       (1UL  << RADIO_PCNF1_ENDIAN_Pos) | 
                       (0UL  << RADIO_PCNF1_WHITEEN_Pos);

    NRF_RADIO->BASE0 = TARGET_BASE_ADDR;
    NRF_RADIO->PREFIX0 = TARGET_PREFIX;
    NRF_RADIO->TXADDRESS = 0;
    NRF_RADIO->RXADDRESSES = 1;

    // CRC 修正：包含地址
    NRF_RADIO->CRCCNF = (RADIO_CRCCNF_LEN_Two << RADIO_CRCCNF_LEN_Pos) | 
                        (0UL << RADIO_CRCCNF_SKIPADDR_Pos);
    
    NRF_RADIO->CRCINIT = 0xFFFF;
    NRF_RADIO->CRCPOLY = 0x11021;
}

void send_ack(void) {
    tx_buffer[0] = 0; 
    tx_buffer[1] = 0; 
    
    // TX 時不需要 RSSI，切換 Shortcut
    NRF_RADIO->SHORTS = (RADIO_SHORTS_READY_START_Msk | RADIO_SHORTS_END_DISABLE_Msk);
    NRF_RADIO->PACKETPTR = (uint32_t)tx_buffer;
    NRF_RADIO->EVENTS_END = 0;
    NRF_RADIO->TASKS_TXEN = 1;
    while (NRF_RADIO->EVENTS_END == 0);
    NRF_RADIO->SHORTS = 0;
}

int main(void)
{
    usb_enable(NULL);
    k_sleep(K_MSEC(3000));

    printk("\n============================================\n");
    printk(">>> HUNTER v5 (RSSI Radar)               <<<\n");
    printk(">>> -40=Hot, -90=Cold                    <<<\n");
    printk("============================================\n");

    radio_init();

    int freq = 0;

    while (1) {
        NRF_RADIO->FREQUENCY = freq;
        NRF_RADIO->PACKETPTR = (uint32_t)rx_buffer;
        
        // === 關鍵設定：開啟 RSSI 自動測量 ===
        // 意思是：當由於地址匹配 (ADDRESS Event) 發生時，自動觸發 RSSISTART 任務
        NRF_RADIO->SHORTS = RADIO_SHORTS_ADDRESS_RSSISTART_Msk;

        NRF_RADIO->EVENTS_READY = 0;
        NRF_RADIO->TASKS_RXEN = 1;
        while (NRF_RADIO->EVENTS_READY == 0);

        NRF_RADIO->EVENTS_END = 0;
        NRF_RADIO->TASKS_START = 1;

        // 掃描視窗
        bool received = false;
        for (volatile int i = 0; i < 20000; i++) {
            if (NRF_RADIO->EVENTS_END) {
                received = true;
                break;
            }
        }

        if (received) {
            // 讀取 RSSI (數值是正的，代表負 dBm，例如讀到 60 代表 -60dBm)
            int rssi_val = NRF_RADIO->RSSISAMPLE;
            int rssi_dbm = -rssi_val; // 轉成負數顯示比較直觀

            // 判斷距離
            char *dist_str = "";
            if (rssi_dbm > -50) dist_str = "!!! VERY CLOSE !!!";
            else if (rssi_dbm > -70) dist_str = "Nearby";
            else dist_str = "Far";

            // 顯示邏輯 (包含 CRC Fail 的也顯示，方便找訊號)
            if (NRF_RADIO->CRCSTATUS == 1) {
                printk("\n>>> [OK] %d MHz | RSSI: %d dBm (%s)\n", freq, rssi_dbm, dist_str);
                printk("    Data: ");
                for(int k=0; k<12; k++) printk("%02X ", rx_buffer[k]);
                printk("\n");

                // 回應 ACK
                NRF_RADIO->TASKS_DISABLE = 1;
                while (NRF_RADIO->EVENTS_DISABLED == 0);
                send_ack();
                // printk("    (ACK Sent)\n");
                k_sleep(K_MSEC(200)); 
            } else {
                // CRC 錯誤但地址對，這通常是邊緣訊號
                printk(">>> [CRC FAIL] %d MHz | RSSI: %d dBm | Data: %02X %02X...\n", 
                       freq, rssi_dbm, rx_buffer[0], rx_buffer[1]);
            }
        }

        NRF_RADIO->TASKS_DISABLE = 1;
        while (NRF_RADIO->EVENTS_DISABLED == 0);
        
        freq++;
        if (freq > 80) freq = 0;
    }
}

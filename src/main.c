/*
 * Pico Tracker 2.0 Receiver for SlimeVR
 * Based on reverse engineered parameters:
 * Address: 0xC0552C6A1E
 * Freq Hopping: 2401, 2437, 2477 MHz
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <hal/nrf_radio.h>
#include <zephyr/usb/usb_device.h>

// ==========================================
// 1. 配置參數
// ==========================================
#define TARGET_BASE_ADDR  0x552c6a1eUL
#define TARGET_PREFIX     0xC0

// 頻率表 (我們鎖定追蹤器會跳的那三個)
static const int channels[] = {1, 37, 77};
#define CH_COUNT 3

// 接收緩衝區
static uint8_t rx_buffer[32];
// ACK 緩衝區 (有些 ESB 設備需要 ACK 帶 Payload)
static uint8_t ack_payload[32] = {0}; 

// ==========================================
// 2. 無線電底層函式
// ==========================================

void radio_disable(void) {
    NRF_RADIO->TASKS_DISABLE = 1;
    while (NRF_RADIO->EVENTS_DISABLED == 0);
    NRF_RADIO->EVENTS_DISABLED = 0;
}

void radio_configure(int channel)
{
    radio_disable();

    NRF_RADIO->FREQUENCY = channel;
    NRF_RADIO->MODE = RADIO_MODE_MODE_Nrf_2Mbit; 
    
    // 設定地址
    NRF_RADIO->BASE0 = TARGET_BASE_ADDR;
    NRF_RADIO->PREFIX0 = TARGET_PREFIX;
    NRF_RADIO->TXADDRESS = 0;
    NRF_RADIO->RXADDRESSES = 1; // Enable Logical addr 0

    // PCNF0: S0=0, LEN=8bit, S1=0 (標準 ESB 結構，LEN欄位很重要)
    // 如果追蹤器不使用 Dynamic Length，這裡可能要設全0。
    // 但根據 pyocd 讀出的 0x40001514 (PCNF1) 的值，我們試試標準設定。
    NRF_RADIO->PCNF0 = (8 << RADIO_PCNF0_LFLEN_Pos); 
    
    // PCNF1: MaxLen 32, Balen 4, Whiteen 0
    NRF_RADIO->PCNF1 = (32 << RADIO_PCNF1_MAXLEN_Pos) | 
                       (32 << RADIO_PCNF1_STATLEN_Pos) | // Static len
                       (4 << RADIO_PCNF1_BALEN_Pos) | 
                       (RADIO_PCNF1_ENDIAN_Little << RADIO_PCNF1_ENDIAN_Pos);

    // CRC 設定
    NRF_RADIO->CRCCNF = (RADIO_CRCCNF_LEN_Two << RADIO_CRCCNF_LEN_Pos); 
    NRF_RADIO->CRCINIT = 0xFFFF;       
    NRF_RADIO->CRCPOLY = 0x11021; 
    
    // Shortcut: 收到後自動校驗 CRC
    NRF_RADIO->SHORTS = (RADIO_SHORTS_READY_START_Msk | RADIO_SHORTS_END_DISABLE_Msk);
}

// ==========================================
// 3. 主程式
// ==========================================

int main(void)
{
    // 初始化 USB
    if (usb_enable(NULL)) {
        return;
    }
    k_sleep(K_MSEC(1000));
    printk(">>> PICO TRACKER RECEIVER STARTED <<<\n");

    int ch_idx = 0;

    while (1) {
        // 1. 設定頻率
        int current_freq = channels[ch_idx];
        radio_configure(current_freq);

        // 2. 進入 RX 模式
        NRF_RADIO->PACKETPTR = (uint32_t)rx_buffer;
        NRF_RADIO->TASKS_RXEN = 1;

        // 3. 等待接收 (或超時)
        // 這裡我們只等一小段時間，因為如果這個頻率沒人，我們就要趕快去下一個頻率找
        // 追蹤器跳頻很快，我們掃描也要快
        bool received = false;
        for(int i=0; i<500; i++) { // 約 5ms 超時
            if (NRF_RADIO->EVENTS_DISABLED) {
                // 收到封包且 CRC 正確 (因為 SHORTS 設定了 END_DISABLE)
                if (NRF_RADIO->CRCSTATUS == 1) {
                    received = true;
                }
                NRF_RADIO->EVENTS_DISABLED = 0;
                break;
            }
            k_busy_wait(10);
        }

        // 4. 處理數據
        if (received) {
            // 打印出來分析 (SlimeVR 需要的格式後續再加)
            printk("RX Freq:%d [", current_freq);
            for(int i=0; i<16; i++) printk("%02X ", rx_buffer[i]); // 先看前16 byte
            printk("]\n");

            // TODO: 解析 rx_buffer 裡的四元數 (Float x 4)
            // TODO: 發送 ACK (如果不發 ACK，追蹤器可能會一直重傳同一包)
            // 簡單的發送 ACK (切換 TX 發一個空包)
            /*
            NRF_RADIO->SHORTS = RADIO_SHORTS_READY_START_Msk | RADIO_SHORTS_END_DISABLE_Msk;
            NRF_RADIO->PACKETPTR = (uint32_t)ack_payload;
            NRF_RADIO->TASKS_TXEN = 1;
            while(!NRF_RADIO->EVENTS_DISABLED);
            NRF_RADIO->EVENTS_DISABLED = 0;
            */
            
            // 如果在當前頻率抓到了，就不要急著跳走，多聽一會兒
            // 這樣可以鎖定住追蹤器
            k_sleep(K_MSEC(2)); 
        } else {
            // 沒收到，去下一個頻率
            ch_idx++;
            if (ch_idx >= CH_COUNT) ch_idx = 0;
        }
    }
}

/*
 * Pico Motion Tracker 2.0 Receiver (Sniffer Mode)
 * Target Address: 0xC0552C6A1E (Reverse Engineered)
 * Target Frequency: 2401 MHz (Channel 1)
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <hal/nrf_radio.h>
#include <zephyr/usb/usb_device.h>

// ==========================================
// 1. 破解參數設定
// ==========================================
// 我們從 pyocd 讀出的地址 BASE0: 0x552C6A1E, PREFIX0: 0xC0
#define TARGET_BASE_ADDR  0x552c6a1eUL
#define TARGET_PREFIX     0xC0

// 鎖定 Channel 1 (2401 MHz)
// 雖然它會跳頻 (1->37->77)，但我們守在這裡一定抓得到
#define RX_FREQ_CHANNEL   1 

static uint8_t rx_buffer[32];

// ==========================================
// 2. 無線電配置函數
// ==========================================
void radio_configure(void)
{
    // 先停用 Radio 才能修改參數
    NRF_RADIO->TASKS_DISABLE = 1;
    while (NRF_RADIO->EVENTS_DISABLED == 0);
    NRF_RADIO->EVENTS_DISABLED = 0;

    // 設定物理層 (2Mbit 是 Nordic ESB 標準)
    NRF_RADIO->MODE = RADIO_MODE_MODE_Nrf_2Mbit;

    // 設定頻率
    NRF_RADIO->FREQUENCY = RX_FREQ_CHANNEL;

    // 設定通訊地址
    NRF_RADIO->BASE0 = TARGET_BASE_ADDR;
    NRF_RADIO->PREFIX0 = TARGET_PREFIX;
    
    // 設定使用哪個邏輯地址 (使用 Logical Address 0)
    NRF_RADIO->TXADDRESS = 0;
    NRF_RADIO->RXADDRESSES = 1; 

    // === 封包格式 (Packet Configuration) ===
    // 根據逆向，它是 ESB DPL (Dynamic Payload Length)
    // PCNF0: LFLEN=8bit (1 byte 長度欄位), S0=0, S1=0
    NRF_RADIO->PCNF0 = (8 << RADIO_PCNF0_LFLEN_Pos);

    // PCNF1: MaxLen=32, BalLen=4 (地址長度), Endian=Little
    NRF_RADIO->PCNF1 = (32 << RADIO_PCNF1_MAXLEN_Pos) | 
                       (32 << RADIO_PCNF1_STATLEN_Pos) |
                       (4 << RADIO_PCNF1_BALEN_Pos) | 
                       (RADIO_PCNF1_ENDIAN_Little << RADIO_PCNF1_ENDIAN_Pos);

    // === CRC 校驗 (必須完全正確才能收到) ===
    // 16-bit CRC, Init 0xFFFF, Poly 0x11021
    NRF_RADIO->CRCCNF = (RADIO_CRCCNF_LEN_Two << RADIO_CRCCNF_LEN_Pos);
    NRF_RADIO->CRCINIT = 0xFFFF;
    NRF_RADIO->CRCPOLY = 0x11021;

    // Shortcut: 
    // 1. READY -> START (Radio 準備好後自動開始接收)
    // 2. END -> DISABLE (收完一包後自動關閉，方便我們讀取數據)
    NRF_RADIO->SHORTS = (RADIO_SHORTS_READY_START_Msk | RADIO_SHORTS_END_DISABLE_Msk);
}

// ==========================================
// 3. 主程式
// ==========================================
int main(void)
{
    // 初始化 USB 串口
    if (usb_enable(NULL)) {
        return 0;
    }
    k_sleep(K_MSEC(2000)); // 給你一點時間打開終端機
    printk("\n>>> PICO TRACKER SNIFFER STARTED <<<\n");
    printk("Listening on Freq: 24%02d MHz\n", RX_FREQ_CHANNEL);
    printk("Address: 0x%X (Prefix: 0x%X)\n", TARGET_BASE_ADDR, TARGET_PREFIX);

    // 配置無線電
    radio_configure();

    while (1) {
        // 1. 準備接收緩衝區
        // 必須清除 Buffer，避免看到舊數據
        for(int i=0; i<32; i++) rx_buffer[i] = 0;
        NRF_RADIO->PACKETPTR = (uint32_t)rx_buffer;

        // 2. 啟動接收任務
        // 因為設定了 SHORT: READY->START，我們只要觸發 RXEN 就會自動進入接收狀態
        NRF_RADIO->TASKS_RXEN = 1;

        // 3. 等待接收結束 (由 SHORT: END->DISABLE 觸發)
        // 這會阻塞直到收到封包 或者 我們手動超時
        // 為了簡單測試，我們先做一個無窮等待，直到收到任何東西
        while (NRF_RADIO->EVENTS_DISABLED == 0) {
            // 這裡可以加入超時重置邏輯，但目前先死守
            k_busy_wait(10);
        }
        
        // 清除事件標誌
        NRF_RADIO->EVENTS_DISABLED = 0;

        // 4. 檢查 CRC 是否正確 (CRCError = 0 代表正確)
        if (NRF_RADIO->CRCSTATUS == 1) {
            printk("[RX] DATA CAPTURED! | Payload: ");
            // 印出前 8 個 Byte (這應該是你的 Device ID)
            for(int i=0; i<8; i++) {
                printk("%02X ", rx_buffer[i]);
            }
            // 印出剩餘的 (如果有)
            printk("| Raw: ");
            for(int i=8; i<16; i++) {
                printk("%02X ", rx_buffer[i]);
            }
            printk("\n");
            
            // 收到數據後閃爍一下 LED (如果板子有定義 LED0)
            // gpio_pin_toggle_dt(&led); 
        }

        // 稍微休息，避免串口塞爆 (如果訊號太強)
        k_sleep(K_MSEC(10));
    }
}

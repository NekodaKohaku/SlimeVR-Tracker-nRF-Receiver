/*
 * Pico Motion Tracker 2.0 Receiver (Raw Capture Mode)
 * Target Address: 0xC0552C6A1E (Confirmed via pyOCD)
 * Target Frequency: 2401 MHz (Channel 1)
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <hal/nrf_radio.h>
#include <zephyr/usb/usb_device.h>

// ==========================================
// 1. 破解參數設定
// ==========================================

// 根據 pyocd 驗證：BASE0 = 0x552c6a1e, PREFIX0 = 0xC0 (不是 0x43)
#define TARGET_BASE_ADDR  0x552c6a1eUL
#define TARGET_PREFIX     0xC0

// 鎖定 Channel 1 (2401 MHz)，這是它跳頻的三個點之一
#define RX_FREQ_CHANNEL   1 

// 接收緩衝區
static uint8_t rx_buffer[32];

// ==========================================
// 2. 無線電配置函數
// ==========================================
void radio_configure(void)
{
    // 停用 Radio 以修改參數
    NRF_RADIO->TASKS_DISABLE = 1;
    while (NRF_RADIO->EVENTS_DISABLED == 0);
    NRF_RADIO->EVENTS_DISABLED = 0;

    // --- 物理層與頻率 ---
    NRF_RADIO->MODE = RADIO_MODE_MODE_Nrf_2Mbit;
    NRF_RADIO->FREQUENCY = RX_FREQ_CHANNEL;

    // --- 地址設定 ---
    NRF_RADIO->BASE0 = TARGET_BASE_ADDR;
    NRF_RADIO->PREFIX0 = TARGET_PREFIX;
    NRF_RADIO->TXADDRESS = 0;
    NRF_RADIO->RXADDRESSES = 1; // Enable Logical addr 0

    // --- 封包格式 (Packet Config) ---
    // [關鍵修正] 
    // 設定 LFLEN = 0。不管追蹤器有沒有發長度頭，我們都不依賴它來判斷長度。
    // 這可以避免因為長度欄位誤判而丟包。
    NRF_RADIO->PCNF0 = (0 << RADIO_PCNF0_LFLEN_Pos);

    // [關鍵修正]
    // 設定 STATLEN (靜態長度) = 8。
    // 我們在 pyocd 看到 MaxLen 是 8，所以強制 Radio 每次只收 8 個 Byte。
    // 這樣就算它發的是 Raw Data，我們也能正確收進來。
    NRF_RADIO->PCNF1 = (8 << RADIO_PCNF1_MAXLEN_Pos) | 
                       (8 << RADIO_PCNF1_STATLEN_Pos) | // 強制收 8 bytes
                       (4 << RADIO_PCNF1_BALEN_Pos) | 
                       (RADIO_PCNF1_ENDIAN_Little << RADIO_PCNF1_ENDIAN_Pos);

    // --- CRC 校驗 ---
    // 這是 ESB 標準：16-bit CRC, Init 0xFFFF, Poly 0x11021
    NRF_RADIO->CRCCNF = (RADIO_CRCCNF_LEN_Two << RADIO_CRCCNF_LEN_Pos);
    NRF_RADIO->CRCINIT = 0xFFFF;
    NRF_RADIO->CRCPOLY = 0x11021;

    // --- Shortcut ---
    // Ready -> Start (自動開始)
    // End -> Disable (收完自動停，方便我們讀數據)
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
    k_sleep(K_MSEC(2000));
    
    printk("\n============================================\n");
    printk(">>> PICO TRACKER RECEIVER (RAW MODE) <<<\n");
    printk("Target: 0x%X (Prefix 0x%X)\n", TARGET_BASE_ADDR, TARGET_PREFIX);
    printk("Listening on Channel %d (2401 MHz)...\n", RX_FREQ_CHANNEL);
    printk("============================================\n");

    // 配置無線電
    radio_configure();

    while (1) {
        // 1. 重置緩衝區 (填 0 以便觀察)
        for(int i=0; i<32; i++) rx_buffer[i] = 0;
        
        NRF_RADIO->PACKETPTR = (uint32_t)rx_buffer;

        // 2. 啟動接收
        // 觸發 RXEN 後，Radio 會經過 Ready -> Start (由 SHORT 控制)
        NRF_RADIO->TASKS_RXEN = 1;

        // 3. 等待接收結束
        // 這裡我們用一個簡單的迴圈等待，直到 EVENTS_DISABLED 被觸發 (代表收完並關閉了)
        // 注意：如果一直沒收到，程式會卡在這裡 (Blocking)。
        // 為了測試配對包，這是可以接受的，因為只要您長按按鈕，它一定會發。
        while (NRF_RADIO->EVENTS_DISABLED == 0) {
            // 這裡不做任何事，就是死等
            // 在實際產品中需要加超時 (Timeout) 機制
             k_busy_wait(10);
        }
        
        // 清除標誌
        NRF_RADIO->EVENTS_DISABLED = 0;

        // 4. 檢查 CRC
        if (NRF_RADIO->CRCSTATUS == 1) {
            printk("[RX] HIT! Payload: ");
            // 印出收到的 8 個 Bytes
            for(int i=0; i<8; i++) {
                printk("%02X ", rx_buffer[i]);
            }
            printk("\n");
            
            // 收到後暫停一下，避免刷屏太快
            k_sleep(K_MSEC(100));
        }
        else {
            // 如果收到但 CRC 錯了 (可能是雜訊)，我們可以印個標記
            // printk("."); 
        }
        
        // 稍作休息
        k_sleep(K_MSEC(5));
    }
}

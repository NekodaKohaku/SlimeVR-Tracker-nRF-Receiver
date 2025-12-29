/*
 * Pico Tracker Sniffer (FINAL GOLDEN VERSION)
 * Target: nRF52810 (Pico Tracker)
 * * [Verified Parameters]
 * MODE:    Ble_2Mbit (0x04)
 * PCNF0:   L=8, S0=0, S1=4 (Critical Fix!)
 * PCNF1:   MaxLen=55, BigEndian, Balen=4
 * ADDRESS: Base=0x552C6A1E, Prefix=0xC0
 * CRC:     16-bit, Poly=0x1021, Init=0xFFFF
 * FREQ:    Hopping {2446, 2454, 2472, 2480}
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <hal/nrf_radio.h>
#include <zephyr/usb/usb_device.h>
#include <zephyr/drivers/uart.h> // 引入 UART 驅動以檢測 DTR

// ==========================================
// 1. 頻率白名單 (根據 pyOCD 觀察到的值)
// ==========================================
// 46=2446, 54=2454, 72=2472, 80=2480 MHz
static const uint8_t target_freqs[] = {46, 54, 72, 80}; 

// ==========================================
// 2. 地址設定 (根據 FICR/Registers 逆向)
// ==========================================
// 邏輯地址: 0xC0552C6A1E (Big Endian Air Order)
#define ADDR_BASE      0x552C6A1EUL
#define ADDR_PREFIX    0xC0

static uint8_t rx_buffer[64]; // 接收緩衝區

void radio_init(void)
{
    // 重置 Radio
    NRF_RADIO->POWER = 0;
    k_busy_wait(500);
    NRF_RADIO->POWER = 1;

    // --- 物理層 ---
    NRF_RADIO->MODE = NRF_RADIO_MODE_BLE_2MBIT; // 0x04

    // --- 封包結構 (PCNF0) ---
    // ★關鍵修正：根據 pyOCD 讀到的 0x00040008
    // LFLEN=8bit, S0=0, S1=4 (必須設為 4，否則位元會錯位)
    NRF_RADIO->PCNF0 = (8UL << RADIO_PCNF0_LFLEN_Pos) | 
                       (0UL << RADIO_PCNF0_S0LEN_Pos) | 
                       (4UL << RADIO_PCNF0_S1LEN_Pos); 

    // --- 封包結構 (PCNF1) ---
    // MaxLen=55, Balen=4, Endian=Big, White=Disabled
    NRF_RADIO->PCNF1 = (55UL << RADIO_PCNF1_MAXLEN_Pos) |
                       (55UL << RADIO_PCNF1_STATLEN_Pos) | 
                       (4UL  << RADIO_PCNF1_BALEN_Pos) |
                       (1UL  << RADIO_PCNF1_ENDIAN_Pos) |  // Big Endian
                       (0UL  << RADIO_PCNF1_WHITEEN_Pos);

    // --- 地址設定 ---
    NRF_RADIO->BASE0 = ADDR_BASE;
    NRF_RADIO->PREFIX0 = (ADDR_PREFIX << 0); // Pipe 0
    
    NRF_RADIO->TXADDRESS = 0;
    NRF_RADIO->RXADDRESSES = 1; // 啟用 Pipe 0

    // --- CRC 設定 ---
    // LEN=2, POLY=0x1021, INIT=0xFFFF
    NRF_RADIO->CRCCNF = (RADIO_CRCCNF_LEN_Two << RADIO_CRCCNF_LEN_Pos) | 
                        (0UL << RADIO_CRCCNF_SKIPADDR_Pos);
    
    NRF_RADIO->CRCINIT = 0xFFFF;
    NRF_RADIO->CRCPOLY = 0x1021; 

    // --- 捷徑 ---
    // 收到封包(END)後自動禁用(DISABLE)，方便 CPU 讀取數據
    NRF_RADIO->SHORTS = RADIO_SHORTS_READY_START_Msk | RADIO_SHORTS_END_DISABLE_Msk;
}

int main(void)
{
    // 取得 USB Serial 裝置指標
    const struct device *dev = device_get_binding("CDC_ACM_0");
    if (!dev) {
        return 0;
    }

    if (usb_enable(NULL)) {
        return 0;
    }

    // ============================================
    // ★ 等待電腦端開啟 Serial Terminal (DTR) ★
    // ============================================
    // Dongle 啟動很快，如果沒有這段，你會錯過前面的 Log
    uint32_t dtr = 0;
    while (!dtr) {
        uart_line_ctrl_get(dev, UART_LINE_CTRL_DTR, &dtr);
        k_sleep(K_MSEC(100)); // 每 0.1 秒檢查一次
    }
    // ============================================

    k_sleep(K_MSEC(1000)); // 連線後稍微緩衝一下

    printk("\n============================================\n");
    printk(">>> Pico Tracker Sniffer (S1=4 Fixed)    <<<\n");
    printk(">>> Connection Established!              <<<\n");
    printk(">>> Listening on 2446, 2454, 2472, 2480  <<<\n");
    printk("============================================\n");

    radio_init();

    int freq_idx = 0;

    while (1) {
        int current_freq = target_freqs[freq_idx];
        NRF_RADIO->FREQUENCY = current_freq;
        
        printk(">>> [Freq %d MHz] Scanning...\n", 2400 + current_freq);

        // 在每個頻率停留 2 秒 (Camp Mode)
        int64_t end_time = k_uptime_get() + 2000;

        while (k_uptime_get() < end_time) {
            
            // 1. 設定 RX Buffer 指標
            NRF_RADIO->PACKETPTR = (uint32_t)rx_buffer;
            
            // 2. 啟動接收 (Shorts 會自動處理 Ready->Start)
            NRF_RADIO->EVENTS_END = 0;
            NRF_RADIO->TASKS_RXEN = 1;

            // 3. 等待接收完成 (Timeout 機制)
            // Tracker 發包極快，這裡短暫等待 20ms
            bool packet_received = false;
            for (int i = 0; i < 20000; i++) {
                if (NRF_RADIO->EVENTS_END) {
                    packet_received = true;
                    break;
                }
                k_busy_wait(1); // 1us busy wait
            }

            if (packet_received) {
                // 檢查 CRC 是否正確
                if (NRF_RADIO->CRCSTATUS == 1) {
                    printk("\n[RX OK] Freq: %d | Data: ", 2400 + current_freq);
                    
                    // 印出前 32 bytes
                    // 預期看到: 1C 00 00 02 ...
                    for(int k=0; k < 32; k++) {
                        printk("%02X ", rx_buffer[k]);
                    }
                    printk("\n");

                    // ★ 抓到了！延長在這個頻率的時間，多抓幾包
                    end_time += 200; 

                } else {
                    // CRC 錯誤 (雜訊或剛好撞到跳頻邊緣)
                    // printk("."); 
                }
                
                NRF_RADIO->EVENTS_END = 0;
                // 因為設定了 SHORTS_END_DISABLE，Radio 此時已經 Disable 了
                // 下一次迴圈會重新 TASKS_RXEN
            } else {
                // 超時沒收到，手動關閉以重置狀態
                NRF_RADIO->TASKS_DISABLE = 1;
                while (NRF_RADIO->EVENTS_DISABLED == 0);
            }
        }

        // 換下一個頻率
        freq_idx++;
        if (freq_idx >= sizeof(target_freqs) / sizeof(target_freqs[0])) {
            freq_idx = 0;
        }
    }
}

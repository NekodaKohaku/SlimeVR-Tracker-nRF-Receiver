/*
 * Pico Tracker HUNTER v7 (Verified Params + USB Fix)
 * 基於你的 Camper Mode 架構修改
 * 頻率：2446, 2454, 2472, 2480 MHz
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <hal/nrf_radio.h>
#include <zephyr/usb/usb_device.h>
#include <zephyr/drivers/uart.h> // ★ 新增：為了偵測 DTR 訊號

// === [修改點 1] 更新為逆向出的正確頻率 ===
// 46=2446, 54=2454, 72=2472, 80=2480 MHz
static const uint8_t target_freqs[] = {46, 54, 72, 80};

// === [修改點 2] 更新為逆向出的正確地址 ===
// 邏輯地址: 0xC0552C6A1E (Big Endian)
#define ADDR_BASE      0x552C6A1EUL
#define ADDR_PREFIX    0xC0

static uint8_t rx_buffer[64];
// static uint8_t tx_buffer[32]; // 先註解掉 TX，抓到 RX 再開啟

void radio_init(void)
{
    NRF_RADIO->POWER = 0;
    k_busy_wait(500);
    NRF_RADIO->POWER = 1;

    NRF_RADIO->MODE = NRF_RADIO_MODE_BLE_2MBIT;

    // === [修改點 3] 封包結構修正 ===
    // PCNF0: L=8, S0=0, S1=4 (這點你原本的程式碼其實是對的！但我們明確寫出來)
    NRF_RADIO->PCNF0 = (8UL << RADIO_PCNF0_LFLEN_Pos) | 
                       (0UL << RADIO_PCNF0_S0LEN_Pos) | 
                       (4UL << RADIO_PCNF0_S1LEN_Pos);

    // PCNF1: MaxLen=55 (放大一點), Endian=Big
    NRF_RADIO->PCNF1 = (55UL << RADIO_PCNF1_MAXLEN_Pos) |
                       (55UL << RADIO_PCNF1_STATLEN_Pos) |
                       (4UL  << RADIO_PCNF1_BALEN_Pos) |
                       (1UL  << RADIO_PCNF1_ENDIAN_Pos) | 
                       (0UL  << RADIO_PCNF1_WHITEEN_Pos);

    // 地址設定
    NRF_RADIO->BASE0 = ADDR_BASE;
    NRF_RADIO->PREFIX0 = (ADDR_PREFIX << 0);
    
    NRF_RADIO->TXADDRESS = 0;
    NRF_RADIO->RXADDRESSES = 1; // 啟用 Pipe 0

    // === [修改點 4] CRC 修正 ===
    // 之前是 0x11021 (錯)，改成 0x1021 (對)
    NRF_RADIO->CRCCNF = (RADIO_CRCCNF_LEN_Two << RADIO_CRCCNF_LEN_Pos) | 
                        (0UL << RADIO_CRCCNF_SKIPADDR_Pos);
    
    NRF_RADIO->CRCINIT = 0xFFFF;
    NRF_RADIO->CRCPOLY = 0x1021; // CCITT
    
    // 捷徑：收到封包後自動 Disable，方便讀取
    NRF_RADIO->SHORTS = RADIO_SHORTS_READY_START_Msk | RADIO_SHORTS_END_DISABLE_Msk;
}

int main(void)
{
    const struct device *dev = device_get_binding("CDC_ACM_0");
    if (!dev) {
        return 0;
    }

    if (usb_enable(NULL)) {
        return 0;
    }

    // ============================================
    // ★ [關鍵修復] 等待電腦端開啟 Serial Terminal ★
    // ============================================
    // 這是解決「訊息不見」的唯一解法。
    // Dongle 會在這裡死循環，直到你打開 PuTTY/串口軟體
    uint32_t dtr = 0;
    while (!dtr) {
        uart_line_ctrl_get(dev, UART_LINE_CTRL_DTR, &dtr);
        k_sleep(K_MSEC(100));
    }
    // ============================================

    k_sleep(K_MSEC(1000)); // 連線後給一點緩衝

    printk("\n============================================\n");
    printk(">>> HUNTER v7 (Params Verified)          <<<\n");
    printk(">>> Waiting for PC... Connected!         <<<\n");
    printk(">>> Freqs: 2446, 2454, 2472, 2480 MHz    <<<\n");
    printk("============================================\n");

    radio_init();

    int freq_idx = 0;

    while (1) {
        int current_freq = target_freqs[freq_idx];
        NRF_RADIO->FREQUENCY = current_freq;
        
        printk(">>> Camping on %d MHz... (Scanning)\n", 2400 + current_freq);

        // 你的策略：在這個頻率駐留 2 秒
        int64_t end_time = k_uptime_get() + 2000;

        while (k_uptime_get() < end_time) {
            
            NRF_RADIO->PACKETPTR = (uint32_t)rx_buffer;
            
            // 啟動接收
            NRF_RADIO->EVENTS_END = 0;
            NRF_RADIO->TASKS_RXEN = 1;

            // 等待接收 (短 Timeout 20ms 以配合快速跳頻)
            bool received = false;
            for (int i = 0; i < 20000; i++) {
                if (NRF_RADIO->EVENTS_END) {
                    received = true;
                    break;
                }
                k_busy_wait(1);
            }

            if (received) {
                // 有收到任何訊號 (不管 CRC)
                if (NRF_RADIO->CRCSTATUS == 1) {
                    // ★ JACKPOT! 抓到正確數據 ★
                    int8_t rssi = -(int8_t)NRF_RADIO->RSSISAMPLE; 
                    printk("\n!!! [JACKPOT] !!! Freq %d | RSSI %d\n", 2400 + current_freq, rssi);
                    printk("Data: ");
                    for(int k=0; k<30; k++) printk("%02X ", rx_buffer[k]);
                    printk("\n");
                    
                    // 抓到了！延長在這個頻率的時間，多看幾眼
                    end_time += 200; 

                    // 註：先不送 ACK，以免干擾。等看到數據確認無誤後再開啟 TX。
                } else {
                    // CRC 失敗 (可能是雜訊或頻率邊緣)
                    // printk("."); 
                }
                
                NRF_RADIO->EVENTS_END = 0;
            } else {
                // 超時沒收到，手動重置
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

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/usb/usb_device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/drivers/gpio.h>
#include <hal/nrf_radio.h>
#include <string.h>

// ---------------------------------------------------------
// 參數定義 (來自逆向工程)
// ---------------------------------------------------------
#define PUBLIC_ADDR      0x552C6A1E
#define PAYLOAD_ID_SALT  0xB9522E32  // PICO 的簽名/鹽值

// 掃描頻道 (2401, 2426, 2480 MHz)
static const uint8_t scan_channels[] = {1, 26, 80};

// Dongle 發送的挑戰包 (模擬 V80 Python 腳本)
// 格式: Header(4) + Payload(Variable) + Salt(4) + PID(1)
// 這就是你要 "喊" 給追蹤器聽的內容
static uint8_t tx_packet[] = {
    // Header (11 02 04 00) - 標準數據包頭
    0x11, 0x02, 0x04, 0x00, 
    // Payload (任意填充數據)
    0x7C, 0x00, 0x00, 0x00, 0x55, 0x00, 0x00, 0x08, 0x80, 0xA4,
    // Salt (關鍵！必須是 B9 52 2E 32 -> Little Endian)
    0x32, 0x2E, 0x52, 0xB9, 
    // PID (之後會動態切換 00/01)
    0x00 
};

// 接收緩衝區 (用來存放追蹤器回傳的 FICR)
static uint8_t rx_buffer[32];

// ---------------------------------------------------------
// 輔助函數
// ---------------------------------------------------------

// ARM RBIT 指令 (反轉位元) - C 語言標準庫沒有，必須用組合語言
static inline uint32_t rbit(uint32_t value) {
    uint32_t result;
    __asm volatile ("rbit %0, %1" : "=r" (result) : "r" (value));
    return result;
}

// 初始化無線電
void radio_init(uint32_t frequency) {
    // 1. 確保無線電處於關閉狀態
    NRF_RADIO->TASKS_DISABLE = 1;
    while (NRF_RADIO->EVENTS_DISABLED == 0);
    NRF_RADIO->EVENTS_DISABLED = 0;

    // 2. 設定物理層
    NRF_RADIO->TXPOWER   = (RADIO_TXPOWER_TXPOWER_0dBm << RADIO_TXPOWER_TXPOWER_Pos);
    NRF_RADIO->FREQUENCY = frequency; 
    NRF_RADIO->MODE      = (RADIO_MODE_MODE_Nrf_2Mbit << RADIO_MODE_MODE_Pos);

    // 3. 設定地址 (Base0 = Public)
    NRF_RADIO->PREFIX0 = 0;
    NRF_RADIO->BASE0   = PUBLIC_ADDR;
    NRF_RADIO->TXADDRESS   = 0; // 發送使用 Logical Address 0
    NRF_RADIO->RXADDRESSES = 1; // 接收啟用 Logical Address 0

    // 4. 設定封包結構 (必須與 PICO 嚴格匹配)
    // LFLEN=8bit, S0LEN=1bit, S1LEN=0
    NRF_RADIO->PCNF0 = (8 << RADIO_PCNF0_LFLEN_Pos) |
                       (1 << RADIO_PCNF0_S0LEN_Pos) |
                       (0 << RADIO_PCNF0_S1LEN_Pos);

    // MaxLen=255, Balen=3 (Base address length 4 bytes), Endian=Little, WhiteEn=1
    NRF_RADIO->PCNF1 = (255 << RADIO_PCNF1_MAXLEN_Pos) |
                       (3   << RADIO_PCNF1_BALEN_Pos) |
                       (RADIO_PCNF1_ENDIAN_Little << RADIO_PCNF1_ENDIAN_Pos) |
                       (1   << RADIO_PCNF1_WHITEEN_Pos); 

    // 5. CRC 設定 (3 bytes)
    NRF_RADIO->CRCCNF = (RADIO_CRCCNF_LEN_Three << RADIO_CRCCNF_LEN_Pos) |
                        (RADIO_CRCCNF_SKIPADDR_Skip << RADIO_CRCCNF_SKIPADDR_Pos);
    NRF_RADIO->CRCINIT = 0xFFFF;
    NRF_RADIO->CRCPOLY = 0x11021; // CRC-CCITT
}

// 計算私有地址 (核心演算法)
// Radio ID = RBIT(FICR + Salt)
uint32_t calculate_pico_address(uint32_t ficr) {
    uint8_t *f = (uint8_t*)&ficr;
    uint32_t salt_val = PAYLOAD_ID_SALT;
    uint8_t *s = (uint8_t*)&salt_val;
    uint8_t res[4];

    // Byte-wise 加法 (模擬 8-bit overflow)
    // FICR: 19 7E A1 F3 ...
    // Salt: 32 2E 52 B9 ...
    for(int i=0; i<4; i++) {
        res[i] = (f[i] + s[i]) & 0xFF;
    }

    // 組合回 32-bit 整數
    uint32_t combined = (res[3] << 24) | (res[2] << 16) | (res[1] << 8) | res[0];
    
    // 執行 RBIT
    return rbit(combined);
}

// ---------------------------------------------------------
// 主邏輯
// ---------------------------------------------------------
void main(void)
{
    // 1. 初始化 USB Stack
    if (usb_enable(NULL)) {
        return;
    }

    // 2. 等待 Serial Terminal 連線 (DTR)
    // 這確保你打開 PuTTY 時能看到第一行字，不會漏掉 Log
    const struct device *dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_console));
    uint32_t dtr = 0;
    
    // 如果想要插電就跑，可以註解掉這個 while 迴圈
    // 如果要除錯，建議保留
    while (!dtr) {
        uart_line_ctrl_get(dev, UART_LINE_CTRL_DTR, &dtr);
        k_sleep(K_MSEC(100));
    }

    printk("\n\n");
    printk("==========================================\n");
    printk("   PICO Tracker Dongle Clone (Zephyr)     \n");
    printk("   Protocol: Active Ping -> Wait for FICR \n");
    printk("==========================================\n");
    k_sleep(K_SECONDS(1));

    int ch_idx = 0;
    bool paired = false;
    uint32_t private_id = 0;

    // === 階段一：配對掃描 (Ping-Pong) ===
    while (!paired) {
        // 切換頻率
        uint8_t current_freq = scan_channels[ch_idx];
        radio_init(current_freq);
        printk("Scanning on %d MHz...\n", 2400 + current_freq);

        // 在每個頻道嘗試發送 20 次請求
        for(int i=0; i<20; i++) {
            
            // --- A. 發送挑戰包 (TX) ---
            NRF_RADIO->PACKETPTR = (uint32_t)tx_packet;
            
            // 設定: 發送完自動關閉 (END -> DISABLE)
            NRF_RADIO->SHORTS = RADIO_SHORTS_READY_START_Msk | RADIO_SHORTS_END_DISABLE_Msk;
            
            NRF_RADIO->EVENTS_END = 0;
            NRF_RADIO->EVENTS_DISABLED = 0;
            
            // 啟動 TX
            NRF_RADIO->TASKS_TXEN = 1;
            
            // 等待發送完成
            while(NRF_RADIO->EVENTS_DISABLED == 0);
            NRF_RADIO->EVENTS_DISABLED = 0;

            // --- B. 快速切換接收 (RX) ---
            // 注意：這裡我們用軟體切換，雖然比 SHORTS 慢一點點，但方便改 PacketPtr
            NRF_RADIO->PACKETPTR = (uint32_t)rx_buffer;
            
            // 設定: 接收完自動停止
            NRF_RADIO->SHORTS = RADIO_SHORTS_READY_START_Msk | RADIO_SHORTS_END_DISABLE_Msk;
            NRF_RADIO->EVENTS_END = 0;
            
            // 啟動 RX
            NRF_RADIO->TASKS_RXEN = 1;
            
            // --- C. 等待 ACK (FICR) ---
            // PICO 回應很快 (通常 < 500us)，我們等待 5ms 即可
            bool packet_received = false;
            for(int w=0; w<5000; w++) { // Busy wait timeout
                if(NRF_RADIO->EVENTS_END) {
                    packet_received = true;
                    break;
                }
                k_busy_wait(1); // 1us wait
            }

            if (packet_received) {
                // 收到封包！檢查 CRC
                if (NRF_RADIO->CRCSTATUS == 1) {
                    // 檢查是否為配對回應包 (長度 0D, Header 0D 02 02 00)
                    // rx_buffer[0] 是 Length (0x0D)
                    if (rx_buffer[0] == 0x0D) {
                        printk("\n[+] Captured Packet! Length: 13\n");
                        printk("    Raw: ");
                        for(int k=0; k<16; k++) printk("%02X ", rx_buffer[k]);
                        printk("\n");

                        // 提取 FICR
                        // 根據 Log: 0D 02 02 00 [19 7E A1 F3] ...
                        // Header(0,1,2,3) -> FICR(4,5,6,7)
                        uint32_t ficr = 0;
                        memcpy(&ficr, &rx_buffer[4], 4); 
                        
                        printk("    Target FICR: %08X\n", ficr);
                        
                        // 計算私有地址
                        private_id = calculate_pico_address(ficr);
                        printk("    >>> Calculated Private ID: %08X <<<\n", private_id);
                        
                        paired = true;
                        break; // 退出 for 迴圈
                    }
                }
            }
            
            // 停止接收 (如果超時)
            NRF_RADIO->TASKS_DISABLE = 1;
            while(NRF_RADIO->EVENTS_DISABLED == 0);
            NRF_RADIO->EVENTS_DISABLED = 0;

            // 切換 PID (模擬真實通訊)
            tx_packet[sizeof(tx_packet)-1] ^= 1;
            
            k_sleep(K_MSEC(10)); // 發送間隔
        }
        
        if (!paired) {
            ch_idx = (ch_idx + 1) % 3; // 換下一個頻道
            k_sleep(K_MSEC(50));
        }
    }

    // === 階段二：連線成功 (進入私有頻道) ===
    printk("\n[Success] Pairing Complete! Listening on Private Channel...\n");
    
    // 重新初始化為私有配置
    // 這裡通常需要更複雜的跳頻邏輯，目前我們先監聽 "算出後的 ID"
    // 注意：實際運作中，追蹤器配對完會跳到數據頻道，可能不是 2401/26/80
    // 但我們會先把 Base1 設好，證明地址正確
    
    NRF_RADIO->TASKS_DISABLE = 1;
    while(NRF_RADIO->EVENTS_DISABLED == 0);

    // 設定私有地址到 BASE1 (Logic Address 1)
    NRF_RADIO->BASE1 = private_id;
    NRF_RADIO->RXADDRESSES = 2; // 只啟用 Logic 1 (BASE1 + PREFIX0[1])

    NRF_RADIO->PACKETPTR = (uint32_t)rx_buffer;
    NRF_RADIO->SHORTS = RADIO_SHORTS_READY_START_Msk | RADIO_SHORTS_END_START_Msk; // 持續接收
    
    NRF_RADIO->TASKS_RXEN = 1;

    while (1) {
        if (NRF_RADIO->EVENTS_END) {
            NRF_RADIO->EVENTS_END = 0;
            if (NRF_RADIO->CRCSTATUS) {
                // 收到 IMU 數據！
                printk("IMU Pkt: %02X %02X %02X %02X...\n", 
                       rx_buffer[0], rx_buffer[1], rx_buffer[2], rx_buffer[3]);
            }
        }
        k_sleep(K_MSEC(1));
    }
}

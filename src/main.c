#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/usb/usb_device.h>
#include <zephyr/drivers/gpio.h>
#include <hal/nrf_radio.h>
#include <string.h>

// ---------------------------------------------------------
// 參數定義 (來自逆向工程)
// ---------------------------------------------------------
#define PUBLIC_ADDR      0x552C6A1E
#define PAYLOAD_ID_SALT  0xB9522E32  // 你的 Payload ID (Little Endian in math)

// Dongle 發送的挑戰包 (模擬 V80)
// 格式: Header(4) + Payload(Variable) + Salt(4)
static uint8_t tx_packet[] = {
    // Header (11 02 04 00)
    0x11, 0x02, 0x04, 0x00, 
    // Payload (任意填充，只要結尾是 Salt)
    0x7C, 0x00, 0x00, 0x00, 0x55, 0x00, 0x00, 0x08, 0x80, 0xA4,
    // Salt (B9 52 2E 32 -> Little Endian stored as 32 2E 52 B9)
    0x32, 0x2E, 0x52, 0xB9, 
    0x01 // PID / Seq
};

// 接收緩衝區
static uint8_t rx_buffer[32];

// ---------------------------------------------------------
// 輔助函數
// ---------------------------------------------------------

// ARM RBIT 指令 (反轉位元)
static uint32_t rbit(uint32_t value) {
    uint32_t result;
    // 使用 ARM 內建指令加速
    __asm volatile ("rbit %0, %1" : "=r" (result) : "r" (value));
    return result;
}

// 初始化無線電 (模擬 Python V80 的設定)
void radio_init(uint32_t frequency) {
    // 1. 關閉無線電
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
    NRF_RADIO->TXADDRESS   = 0; // 使用 Base0 發送
    NRF_RADIO->RXADDRESSES = 1; // 使用 Base0 接收

    // 4. 設定封包結構 (PCNF0/1) - 必須與 PICO 嚴格匹配
    // LFLEN=8bit, S0LEN=1bit, S1LEN=0
    NRF_RADIO->PCNF0 = (8 << RADIO_PCNF0_LFLEN_Pos) |
                       (1 << RADIO_PCNF0_S0LEN_Pos) |
                       (0 << RADIO_PCNF0_S1LEN_Pos);

    // MaxLen=255, Balen=3 (Base address length 4 bytes total), Endian=Little
    NRF_RADIO->PCNF1 = (255 << RADIO_PCNF1_MAXLEN_Pos) |
                       (3   << RADIO_PCNF1_BALEN_Pos) |
                       (RADIO_PCNF1_ENDIAN_Little << RADIO_PCNF1_ENDIAN_Pos) |
                       (1   << RADIO_PCNF1_WHITEEN_Pos); // 如果有開 Whitening 就要加，通常 PICO 有開

    // 5. CRC 設定 (3 bytes)
    NRF_RADIO->CRCCNF = (RADIO_CRCCNF_LEN_Three << RADIO_CRCCNF_LEN_Pos) |
                        (RADIO_CRCCNF_SKIPADDR_Skip << RADIO_CRCCNF_SKIPADDR_Pos);
    NRF_RADIO->CRCINIT = 0xFFFF; // 通常初始值
    NRF_RADIO->CRCPOLY = 0x11021; // CRC-CCITT (需要確認，通常 nRF 預設)
}

// 計算私有地址算法
uint32_t calculate_pico_address(uint32_t ficr) {
    // 1. FICR + Salt (Byte-wise addition)
    // 為了方便，我們把它當作 byte array 處理
    uint8_t *f = (uint8_t*)&ficr;
    uint8_t *s = (uint8_t*)&(uint32_t){PAYLOAD_ID_SALT}; // 0xB9522E32
    uint8_t res[4];

    // 注意：這裡是 Little Endian 記憶體佈局
    // FICR: 19 7E A1 F3 (0xF3A17E19)
    // Salt: 32 2E 52 B9 (0xB9522E32)
    
    for(int i=0; i<4; i++) {
        res[i] = (f[i] + s[i]) & 0xFF;
    }

    // 組合回 32-bit (因為我們要 RBIT，這裡要小心 Endian)
    // 根據 Python 邏輯：AC F3 AC 4B -> RBIT -> D2 35 CF 35
    // 我們需要構建 0x4BACF3AC 讓 RBIT 翻轉成 0xD235CF35 ?
    // 不，RBIT 是 bit 級別反轉。
    
    // 讓我們直接用整數加法模擬 Python 的 byte-wise 行為
    // Python: AC F3 AC 4B (Big Endian representation of calculation result)
    // 我們要構造這個整數
    uint32_t combined = (res[3] << 24) | (res[2] << 16) | (res[1] << 8) | res[0];
    
    // 執行 RBIT
    return rbit(combined);
}

// ---------------------------------------------------------
// 主邏輯
// ---------------------------------------------------------
void main(void)
{
    // 初始化 USB (看 Log 用)
    if (usb_enable(NULL)) {
        return;
    }
    k_sleep(K_SECONDS(1)); // 等 USB 連上
    printk("=== PICO Dongle Clone Starting ===\n");

    // 配對用的頻率列表 (掃描這三個)
    uint8_t channels[] = {1, 26, 80}; // 2401, 2426, 2480 MHz
    int ch_idx = 0;

    bool paired = false;
    uint32_t private_id = 0;

    // --- 階段一：配對 (Ping-Pong Loop) ---
    while (!paired) {
        // 設定頻率
        radio_init(channels[ch_idx]);
        printk("Scanning on Channel %d...\n", channels[ch_idx]);

        // 嘗試發送 10 次請求
        for(int i=0; i<10; i++) {
            // 1. 設定 TX Buffer
            NRF_RADIO->PACKETPTR = (uint32_t)tx_packet;
            
            // 2. 設定 SHORTS (TX -> RX 自動切換)
            // READY->START (自動發)
            // END->DISABLE (發完關)
            // DISABLED->RXEN (關完開收 - 抓 ACK)
            NRF_RADIO->SHORTS = RADIO_SHORTS_READY_START_Msk | 
                                RADIO_SHORTS_END_DISABLE_Msk | 
                                RADIO_SHORTS_DISABLED_RXEN_Msk;

            // 清除事件
            NRF_RADIO->EVENTS_END = 0;
            NRF_RADIO->EVENTS_DISABLED = 0;

            // 3. 啟動發送 (觸發整個 SHORTS 鏈)
            NRF_RADIO->TASKS_TXEN = 1;

            // 4. 等待發送完成，並進入 RX 狀態
            // 我們這裡給它一點時間自動切換
            while(NRF_RADIO->EVENTS_DISABLED == 0); // TX 完成
            NRF_RADIO->EVENTS_DISABLED = 0;
            
            // 此時硬體應該已經自動跳到 RXEN -> READY -> START (因為 SHORTS 沒設 RX 的停止)
            // 我們手動設定 RX 的接收 Buffer (最好在 TX 前設好，但這裡為了簡單)
            NRF_RADIO->PACKETPTR = (uint32_t)rx_buffer;
            
            // 啟動 RX (如果 SHORTS 沒自動幫我們 Start，手動補一腳)
            if (NRF_RADIO->STATE != RADIO_STATE_STATE_Rx) {
                NRF_RADIO->TASKS_START = 1; 
            }

            // 5. 聽 10ms 等 ACK
            k_busy_wait(10000); 

            if (NRF_RADIO->EVENTS_END) {
                // 收到東西了！
                if (NRF_RADIO->CRCSTATUS == 1) { // CRC 正確
                    // 檢查封包特徵 (Header 0D 02 02 00)
                    // rx_buffer[0] 是長度
                    if (rx_buffer[0] == 0x0D) {
                        printk("Captured Packet! Length: 13\n");
                        
                        // 提取 FICR (Offset 4, 5, 6, 7)
                        // rx_buffer: [Len] [S0] [S1] [F0] [F1] [F2] [F3] ...
                        //             0    1    2    3    4    5    6    
                        // 注意 S0, S1 位元偏移，通常 RAM 裡 Byte 3 開始是 Payload
                        // 假設: Len(0), S0(1), S1(2), Payload[0]...
                        // 根據你的 Dump: 0D 02 02 00 19 ...
                        // Len=0D, S0=02, S1=00(padded?)
                        // 實際上 nRF EasyDMA 會把 Header 分開還是合再一起取決於設定
                        // 簡單起見，我們掃描 buffer 找 FICR 特徵
                        
                        // 假設 FICR 在 buffer[4] ~ buffer[7]
                        uint32_t ficr = 0;
                        memcpy(&ficr, &rx_buffer[4], 4); // 19 7E A1 F3 -> 0xF3A17E19
                        
                        printk("Found FICR: %08X\n", ficr);
                        
                        // 計算地址
                        private_id = calculate_pico_address(ficr);
                        printk("Calculated Private ID: %08X\n", private_id);
                        
                        paired = true;
                        break;
                    }
                }
                // 停止接收，準備下一次 Ping
                NRF_RADIO->TASKS_DISABLE = 1;
                while(NRF_RADIO->EVENTS_DISABLED == 0);
                NRF_RADIO->EVENTS_DISABLED = 0;
            } else {
                 // 超時沒收到，停止
                NRF_RADIO->TASKS_DISABLE = 1;
                while(NRF_RADIO->EVENTS_DISABLED == 0);
                NRF_RADIO->EVENTS_DISABLED = 0;
            }
            
            // 換下一個 PID (模擬)
            tx_packet[sizeof(tx_packet)-1] ^= 1;
            k_sleep(K_MSEC(10));
        }
        
        if (!paired) {
            ch_idx = (ch_idx + 1) % 3; // 換頻道
        }
    }

    // --- 階段二：正式連線 (IMU 接收) ---
    printk(">>> Entering Private Mode: %08X <<<\n", private_id);
    
    // 重新初始化為私有配置
    NRF_RADIO->TASKS_DISABLE = 1;
    while(NRF_RADIO->EVENTS_DISABLED == 0);
    
    NRF_RADIO->BASE1 = private_id;
    NRF_RADIO->RXADDRESSES = 2; // Enable Base1 (Logical Address 1)
    
    // 這裡通常要設定頻率跳頻表 (Hopping)，目前先停在配對頻道測試
    NRF_RADIO->SHORTS = RADIO_SHORTS_READY_START_Msk | RADIO_SHORTS_END_START_Msk; // 持續接收
    NRF_RADIO->PACKETPTR = (uint32_t)rx_buffer;
    NRF_RADIO->TASKS_RXEN = 1;

    while (1) {
        if (NRF_RADIO->EVENTS_END) {
            NRF_RADIO->EVENTS_END = 0;
            if (NRF_RADIO->CRCSTATUS) {
                printk("IMU Data: %02X %02X %02X...\n", rx_buffer[0], rx_buffer[1], rx_buffer[2]);
            }
        }
        k_yield();
    }
}

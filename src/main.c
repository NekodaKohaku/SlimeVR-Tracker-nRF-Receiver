#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/usb/usb_device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/drivers/gpio.h>
#include <hal/nrf_radio.h>
#include <string.h>

// ---------------------------------------------------------
// 參數定義
// ---------------------------------------------------------
#define PUBLIC_ADDR      0x552C6A1E
#define PAYLOAD_ID_SALT  0xB9522E32  // PICO 簽名

// 掃描頻道 (2401, 2426, 2480 MHz)
static const uint8_t scan_channels[] = {1, 26, 80};

// Dongle 發送的挑戰包
// 格式: Header(4) + Payload... + Salt(4) + PID(1)
static uint8_t tx_packet[] = {
    0x11, 0x02, 0x04, 0x00, 
    0x7C, 0x00, 0x00, 0x00, 0x55, 0x00, 0x00, 0x08, 0x80, 0xA4,
    0x32, 0x2E, 0x52, 0xB9, // Salt (Little Endian)
    0x00 // PID
};

static uint8_t rx_buffer[32];

// ---------------------------------------------------------
// 輔助函數
// ---------------------------------------------------------

// ARM RBIT 指令
static inline uint32_t rbit(uint32_t value) {
    uint32_t result;
    __asm volatile ("rbit %0, %1" : "=r" (result) : "r" (value));
    return result;
}

// 初始化無線電
void radio_init(uint32_t frequency) {
    NRF_RADIO->TASKS_DISABLE = 1;
    while (NRF_RADIO->EVENTS_DISABLED == 0);
    NRF_RADIO->EVENTS_DISABLED = 0;

    NRF_RADIO->TXPOWER   = (RADIO_TXPOWER_TXPOWER_0dBm << RADIO_TXPOWER_TXPOWER_Pos);
    NRF_RADIO->FREQUENCY = frequency; 
    NRF_RADIO->MODE      = (RADIO_MODE_MODE_Nrf_2Mbit << RADIO_MODE_MODE_Pos);

    NRF_RADIO->PREFIX0 = 0;
    NRF_RADIO->BASE0   = PUBLIC_ADDR;
    NRF_RADIO->TXADDRESS   = 0; 
    NRF_RADIO->RXADDRESSES = 1; 

    // PCNF0: LFLEN=8bit, S0LEN=1bit
    NRF_RADIO->PCNF0 = (8 << RADIO_PCNF0_LFLEN_Pos) | (1 << RADIO_PCNF0_S0LEN_Pos);

    // PCNF1: MaxLen=255, Balen=3, Little Endian, Whitening Enable
    NRF_RADIO->PCNF1 = (255 << RADIO_PCNF1_MAXLEN_Pos) |
                       (3   << RADIO_PCNF1_BALEN_Pos) |
                       (RADIO_PCNF1_ENDIAN_Little << RADIO_PCNF1_ENDIAN_Pos) |
                       (1   << RADIO_PCNF1_WHITEEN_Pos); 

    // CRC 3 bytes
    NRF_RADIO->CRCCNF = (RADIO_CRCCNF_LEN_Three << RADIO_CRCCNF_LEN_Pos) |
                        (RADIO_CRCCNF_SKIPADDR_Skip << RADIO_CRCCNF_SKIPADDR_Pos);
    NRF_RADIO->CRCINIT = 0xFFFF;
    NRF_RADIO->CRCPOLY = 0x11021;
}

// 計算私有地址
uint32_t calculate_pico_address(uint32_t ficr) {
    uint8_t *f = (uint8_t*)&ficr;
    uint32_t salt_val = PAYLOAD_ID_SALT;
    uint8_t *s = (uint8_t*)&salt_val;
    uint8_t res[4];

    for(int i=0; i<4; i++) {
        res[i] = (f[i] + s[i]) & 0xFF;
    }
    uint32_t combined = (res[3] << 24) | (res[2] << 16) | (res[1] << 8) | res[0];
    return rbit(combined);
}

// ---------------------------------------------------------
// 主邏輯
// ---------------------------------------------------------
void main(void)
{
    // 1. 初始化 USB
    if (usb_enable(NULL)) {
        return;
    }

    // 2. 【關鍵修改】不再死等 DTR，改用倒數計時
    // 這樣就算 TeraTerm 沒開好，程式也會繼續跑，你可以看燈號或之後的 Log
    for(int i=5; i>0; i--) {
        printk(">>> Starting in %d seconds... (Please open TeraTerm) <<<\n", i);
        k_sleep(K_SECONDS(1));
    }

    printk("\n");
    printk("==========================================\n");
    printk("   PICO Tracker Dongle Clone (ACTIVE)     \n");
    printk("==========================================\n");

    int ch_idx = 0;
    bool paired = false;
    uint32_t private_id = 0;

    // === 階段一：配對掃描 ===
    while (!paired) {
        uint8_t current_freq = scan_channels[ch_idx];
        radio_init(current_freq);
        printk("Scanning on %d MHz...\n", 2400 + current_freq);

        // 每個頻道嘗試發送 20 次
        for(int i=0; i<20; i++) {
            
            // --- A. 發送 (TX) ---
            NRF_RADIO->PACKETPTR = (uint32_t)tx_packet;
            NRF_RADIO->SHORTS = RADIO_SHORTS_READY_START_Msk | RADIO_SHORTS_END_DISABLE_Msk;
            NRF_RADIO->EVENTS_END = 0;
            NRF_RADIO->EVENTS_DISABLED = 0;
            
            NRF_RADIO->TASKS_TXEN = 1;
            while(NRF_RADIO->EVENTS_DISABLED == 0); // 等待發送完成
            NRF_RADIO->EVENTS_DISABLED = 0;

            // --- B. 接收 (RX) ---
            NRF_RADIO->PACKETPTR = (uint32_t)rx_buffer;
            NRF_RADIO->SHORTS = RADIO_SHORTS_READY_START_Msk | RADIO_SHORTS_END_DISABLE_Msk;
            NRF_RADIO->EVENTS_END = 0;
            
            NRF_RADIO->TASKS_RXEN = 1;
            
            // --- C. 等待回應 (Timeout) ---
            bool packet_received = false;
            // 等待約 5ms
            for(int w=0; w<5000; w++) { 
                if(NRF_RADIO->EVENTS_END) {
                    packet_received = true;
                    break;
                }
                k_busy_wait(1);
            }

            if (packet_received) {
                if (NRF_RADIO->CRCSTATUS == 1) {
                    // 檢查 Header 0D... (rx_buffer[0] 為長度)
                    if (rx_buffer[0] == 0x0D) {
                        printk("\n[+] Captured Packet! Length: 13\n");
                        printk("    Raw: ");
                        for(int k=0; k<16; k++) printk("%02X ", rx_buffer[k]);
                        printk("\n");

                        // 提取 FICR (假設 Offset 4)
                        uint32_t ficr = 0;
                        memcpy(&ficr, &rx_buffer[4], 4); 
                        
                        printk("    Target FICR: %08X\n", ficr);
                        
                        private_id = calculate_pico_address(ficr);
                        printk("    >>> Calculated Private ID: %08X <<<\n", private_id);
                        
                        paired = true;
                        break; 
                    }
                }
            }
            
            // 停止接收
            NRF_RADIO->TASKS_DISABLE = 1;
            while(NRF_RADIO->EVENTS_DISABLED == 0);
            NRF_RADIO->EVENTS_DISABLED = 0;

            // 切換 PID
            tx_packet[sizeof(tx_packet)-1] ^= 1;
            k_sleep(K_MSEC(10));
        }
        
        if (!paired) {
            ch_idx = (ch_idx + 1) % 3;
            k_sleep(K_MSEC(50));
        }
    }

    // === 階段二：連線成功 ===
    printk("\n[Success] Pairing Complete! Listening on Private Channel...\n");
    
    NRF_RADIO->TASKS_DISABLE = 1;
    while(NRF_RADIO->EVENTS_DISABLED == 0);

    NRF_RADIO->BASE1 = private_id;
    NRF_RADIO->RXADDRESSES = 2; // Enable Base1

    NRF_RADIO->PACKETPTR = (uint32_t)rx_buffer;
    NRF_RADIO->SHORTS = RADIO_SHORTS_READY_START_Msk | RADIO_SHORTS_END_START_Msk;
    
    NRF_RADIO->TASKS_RXEN = 1;

    // 持續接收資料
    while (1) {
        if (NRF_RADIO->EVENTS_END) {
            NRF_RADIO->EVENTS_END = 0;
            if (NRF_RADIO->CRCSTATUS) {
                // 印出前幾個 Byte 確認有收到資料
                printk("IMU Pkt: %02X %02X %02X %02X\n", 
                       rx_buffer[0], rx_buffer[1], rx_buffer[2], rx_buffer[3]);
            }
        }
        k_sleep(K_MSEC(1));
    }
}

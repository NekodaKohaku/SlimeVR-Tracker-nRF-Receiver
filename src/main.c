/*
 * Pico Tracker ACTIVE PINGER
 * Strategy: 
 * 1. Configure Radio with confirmed settings (Mode 4, Big Endian).
 * 2. SEND a packet to address C0 55 2C 6A 1E.
 * 3. LISTEN for an ACK (which should contain IMU data).
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <hal/nrf_radio.h>
#include <zephyr/usb/usb_device.h>

// === 已確認的硬體參數 ===
#define CH_FREQ           1
#define TARGET_BASE_ADDR  0x552c6a1eUL
#define TARGET_PREFIX     0xC0

static uint8_t tx_buffer[32]; // 發送緩衝區
static uint8_t rx_buffer[32]; // 接收緩衝區

void radio_configure(void)
{
    NRF_RADIO->TASKS_DISABLE = 1;
    k_busy_wait(200);
    NRF_RADIO->EVENTS_DISABLED = 0;

    // 1. [確認] 物理層 Mode 4 (Ble_2Mbit)
    NRF_RADIO->MODE = 4; 
    NRF_RADIO->FREQUENCY = CH_FREQ;

    // 2. [確認] Big Endian + 正常地址長度
    // 這次我們要發射，所以必須設對 BALEN，不能用盲收了
    NRF_RADIO->PCNF0 = (8 << RADIO_PCNF0_LFLEN_Pos) |
                       (0 << RADIO_PCNF0_S0LEN_Pos) |
                       (4 << RADIO_PCNF0_S1LEN_Pos);

    NRF_RADIO->PCNF1 = (32 << RADIO_PCNF1_MAXLEN_Pos) | 
                       (0  << RADIO_PCNF1_STATLEN_Pos) | 
                       (4  << RADIO_PCNF1_BALEN_Pos) | 
                       (1  << RADIO_PCNF1_ENDIAN_Pos); // Big Endian

    // 3. 設定地址
    NRF_RADIO->BASE0 = TARGET_BASE_ADDR;
    NRF_RADIO->PREFIX0 = TARGET_PREFIX;
    
    // TX 和 RX 都用 Address 0
    NRF_RADIO->TXADDRESS = 0;
    NRF_RADIO->RXADDRESSES = 1;

    // 4. CRC (Tracker 有開 CRC，我們也要開，不然它會拒收)
    NRF_RADIO->CRCCNF = (RADIO_CRCCNF_LEN_Two << RADIO_CRCCNF_LEN_Pos);
    NRF_RADIO->CRCINIT = 0xFFFF;
    NRF_RADIO->CRCPOLY = 0x11021;

    // 5. 設定捷徑：發射完 -> 自動轉接收 (等待 ACK)
    // 這是 ESB 的標準操作：Ping -> Wait for Ack
    NRF_RADIO->SHORTS = (RADIO_SHORTS_READY_START_Msk | 
                         RADIO_SHORTS_END_DISABLE_Msk | 
                         RADIO_SHORTS_DISABLED_RXEN_Msk); 
                         // 注意：標準 ESB 是 TX->RX，這裡我們手動切換比較穩，先只用 Ready->Start
    NRF_RADIO->SHORTS = RADIO_SHORTS_READY_START_Msk;
}

int main(void)
{
    usb_enable(NULL);
    k_sleep(K_MSEC(2000));

    printk("\n============================================\n");
    printk(">>> ACTIVE PINGER STARTED                <<<\n");
    printk(">>> Sending PING to C0 55 2C 6A 1E...    <<<\n");
    printk("============================================\n");

    radio_configure();

    // 準備一個簡單的 Payload (空的，或者隨便填一點)
    // 根據 LFLEN=8, S1LEN=4，我們需要填 Header
    // 這裡我們模擬一個空的 Payload，長度為 0
    tx_buffer[0] = 0; // Length = 0
    tx_buffer[1] = 0; // S1 (padding)

    while (1) {
        // === 步驟 1: 發射 Ping ===
        NRF_RADIO->EVENTS_END = 0;
        NRF_RADIO->PACKETPTR = (uint32_t)tx_buffer;
        
        // 切換到 TX 模式
        NRF_RADIO->TASKS_TXEN = 1;
        
        // 等待發射完成
        while (NRF_RADIO->EVENTS_END == 0);
        NRF_RADIO->EVENTS_END = 0;
        
        // 發射完，關閉 Radio (準備切換 RX)
        NRF_RADIO->TASKS_DISABLE = 1;
        while (NRF_RADIO->EVENTS_DISABLED == 0);
        NRF_RADIO->EVENTS_DISABLED = 0;

        // printk("Ping sent... Listening for ACK...\n");

        // === 步驟 2: 聽 ACK (聽 10ms) ===
        NRF_RADIO->PACKETPTR = (uint32_t)rx_buffer;
        NRF_RADIO->TASKS_RXEN = 1;
        
        // 我們給它 5ms 的時間回應
        bool ack_received = false;
        for(int i=0; i<50; i++) {
            if (NRF_RADIO->EVENTS_END) {
                NRF_RADIO->EVENTS_END = 0;
                if (NRF_RADIO->CRCSTATUS == 1) {
                    ack_received = true;
                    break;
                }
                // 如果收到雜訊，繼續聽
                NRF_RADIO->TASKS_START = 1;
            }
            k_busy_wait(100); // 0.1ms
        }

        // === 結果判定 ===
        if (ack_received) {
            printk("\n>>> [ALIVE!] ACK RECEIVED from Tracker! <<<\n");
            printk("Data: ");
            for(int k=0; k<16; k++) printk("%02X ", rx_buffer[k]);
            printk("\n");
        } else {
            // printk("."); // 沒回應
        }

        // 關閉 Radio，休息一下再 Ping
        NRF_RADIO->TASKS_DISABLE = 1;
        while (NRF_RADIO->EVENTS_DISABLED == 0);
        
        k_sleep(K_MSEC(100)); // 每 100ms Ping 一次
    }
}

/*
 * Pico Tracker HUNTER (DEBUG MODE)
 * 修正點：S1LEN=4, 開啟 CRC 錯誤顯示
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <hal/nrf_radio.h>
#include <zephyr/usb/usb_device.h>

static const uint8_t scan_channels[] = {1, 2, 26, 80, 77, 25, 47};

#define TARGET_BASE_ADDR  0x552c6a1eUL
#define TARGET_PREFIX     0xC0

static uint8_t rx_buffer[32];
static uint8_t tx_buffer[32];

void radio_init(void)
{
    NRF_RADIO->POWER = 0;
    k_busy_wait(500);
    NRF_RADIO->POWER = 1;

    NRF_RADIO->MODE = NRF_RADIO_MODE_BLE_2MBIT;

    // === 關鍵修正 ===
    // S1LEN 必須設為 4 (bits)，否則 Payload 會錯位！
    NRF_RADIO->PCNF0 = (8UL << RADIO_PCNF0_LFLEN_Pos) | 
                       (1UL << RADIO_PCNF0_S0LEN_Pos) | 
                       (4UL << RADIO_PCNF0_S1LEN_Pos); // 修正這裡！

    NRF_RADIO->PCNF1 = (32UL << RADIO_PCNF1_MAXLEN_Pos) |
                       (0UL  << RADIO_PCNF1_STATLEN_Pos) |
                       (4UL  << RADIO_PCNF1_BALEN_Pos) |
                       (1UL  << RADIO_PCNF1_ENDIAN_Pos) |
                       (0UL  << RADIO_PCNF1_WHITEEN_Pos);

    NRF_RADIO->BASE0 = TARGET_BASE_ADDR;
    NRF_RADIO->PREFIX0 = TARGET_PREFIX;
    NRF_RADIO->TXADDRESS = 0;
    NRF_RADIO->RXADDRESSES = 1;

    NRF_RADIO->CRCCNF = (RADIO_CRCCNF_LEN_Two << RADIO_CRCCNF_LEN_Pos) | 
                        (RADIO_CRCCNF_SKIPADDR_Skip << RADIO_CRCCNF_SKIPADDR_Pos);
    NRF_RADIO->CRCINIT = 0xFFFF;
    NRF_RADIO->CRCPOLY = 0x11021;

    // 暫時只用 RX，收到確認無誤後再開啟自動 TX
    NRF_RADIO->SHORTS = 0; 
}

void prepare_response_packet(void)
{
    tx_buffer[0] = 0x01; // Header
    tx_buffer[1] = 0x00; // Length
}

void send_ack(void) {
    NRF_RADIO->SHORTS = (RADIO_SHORTS_READY_START_Msk | RADIO_SHORTS_END_DISABLE_Msk);
    NRF_RADIO->PACKETPTR = (uint32_t)tx_buffer;
    NRF_RADIO->EVENTS_END = 0;
    NRF_RADIO->TASKS_TXEN = 1;
    while (NRF_RADIO->EVENTS_END == 0);
    NRF_RADIO->SHORTS = 0; // 重置 Shorts
}

int main(void)
{
    usb_enable(NULL);
    k_sleep(K_MSEC(3000));

    printk("\n============================================\n");
    printk(">>> HUNTER DEBUG MODE STARTED            <<<\n");
    printk(">>> S1LEN=4, Show CRC Errors             <<<\n");
    printk("============================================\n");

    radio_init();
    prepare_response_packet();

    int ch_idx = 0;

    while (1) {
        NRF_RADIO->FREQUENCY = scan_channels[ch_idx];
        
        // 進入 RX
        NRF_RADIO->PACKETPTR = (uint32_t)rx_buffer;
        NRF_RADIO->EVENTS_READY = 0;
        NRF_RADIO->TASKS_RXEN = 1;
        while (NRF_RADIO->EVENTS_READY == 0);

        NRF_RADIO->EVENTS_END = 0;
        NRF_RADIO->TASKS_START = 1;

        // 加長等待時間到 50ms (確保涵蓋一個廣播間隔)
        bool packet_received = false;
        for (volatile int i = 0; i < 40000; i++) {
            if (NRF_RADIO->EVENTS_END) {
                packet_received = true;
                break;
            }
        }

        if (packet_received) {
            if (NRF_RADIO->CRCSTATUS == 1) {
                // === 成功抓到！ ===
                printk("\n>>> [SUCCESS] CH %d | Header:%02X Len:%02X <<<\n", 
                       scan_channels[ch_idx], rx_buffer[0], rx_buffer[1]);
                
                // 嘗試印出 Payload
                printk("Payload: ");
                for(int k=0; k<8; k++) printk("%02X ", rx_buffer[2+k]);
                printk("\n");

                // 只有特徵符合才回 ACK
                if (rx_buffer[0] == 0x42) {
                    printk("Target Identified! Sending ACK...\n");
                    NRF_RADIO->TASKS_DISABLE = 1;
                    while (NRF_RADIO->EVENTS_DISABLED == 0);
                    send_ack();
                    k_sleep(K_MSEC(100)); // 讓它冷靜一下
                }

            } else {
                // === CRC 錯誤 (但地址匹配) ===
                // 這代表我們已經「鎖定」了 Tracker，只是格式還有一點不對
                printk("!"); // 印個驚嘆號代表有訊號
                // printk("[CRC ERR] CH %d \n", scan_channels[ch_idx]); // 想看詳細可以取消註解
            }
        } else {
            // printk("."); // 沒訊號印個點
        }

        // 強制關閉以換頻
        NRF_RADIO->TASKS_DISABLE = 1;
        while (NRF_RADIO->EVENTS_DISABLED == 0);

        ch_idx = (ch_idx + 1) % sizeof(scan_channels);
    }
}

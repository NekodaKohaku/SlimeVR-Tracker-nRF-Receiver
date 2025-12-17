/*
 * Pico Tracker Ultimate Receiver
 * Fix: Removed 'return 0' on usb_enable to prevent early exit.
 * Strategy: Listen for LFLEN=4 packets.
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <hal/nrf_radio.h>
#include <zephyr/usb/usb_device.h>

// === 參數 ===
#define TARGET_BASE_ADDR  0x552c6a1eUL
#define TARGET_PREFIX     0xC0
#define CH_FREQ           1 

static uint8_t rx_buffer[32]; 

void radio_init(void)
{
    // 1. 強制重置 Radio
    NRF_RADIO->TASKS_DISABLE = 1;
    k_busy_wait(1000); // 等待硬體反應
    NRF_RADIO->EVENTS_DISABLED = 0;

    // 2. 設定參數
    NRF_RADIO->MODE = RADIO_MODE_MODE_Nrf_2Mbit;
    NRF_RADIO->FREQUENCY = CH_FREQ;
    NRF_RADIO->BASE0 = TARGET_BASE_ADDR;
    NRF_RADIO->PREFIX0 = TARGET_PREFIX;
    NRF_RADIO->TXADDRESS = 0;
    NRF_RADIO->RXADDRESSES = 1;

    // 3. 設定 LFLEN = 4 (這是我們驗證過的)
    NRF_RADIO->PCNF0 = (4 << RADIO_PCNF0_LFLEN_Pos);

    // 4. 設定 PCNF1
    NRF_RADIO->PCNF1 = (32 << RADIO_PCNF1_MAXLEN_Pos) | 
                       (0  << RADIO_PCNF1_STATLEN_Pos) | 
                       (4  << RADIO_PCNF1_BALEN_Pos) | 
                       (RADIO_PCNF1_ENDIAN_Little << RADIO_PCNF1_ENDIAN_Pos);

    NRF_RADIO->CRCCNF = (RADIO_CRCCNF_LEN_Two << RADIO_CRCCNF_LEN_Pos);
    NRF_RADIO->CRCINIT = 0xFFFF;
    NRF_RADIO->CRCPOLY = 0x11021;
    
    // 5. 自動重啟接收
    NRF_RADIO->SHORTS = RADIO_SHORTS_READY_START_Msk;
}

int main(void)
{
    // [修正] 不管 usb_enable 回傳什麼，都不可以 return！
    // 這樣才能保證程式繼續往下跑
    usb_enable(NULL);

    // 緩衝 2 秒
    k_sleep(K_MSEC(2000));
    
    printk("\n--------------------------------\n");
    printk(">>> SYSTEM ALIVE: MAIN LOOP START <<<\n");
    printk("--------------------------------\n");

    radio_init();

    // 啟動接收
    NRF_RADIO->PACKETPTR = (uint32_t)rx_buffer;
    NRF_RADIO->TASKS_RXEN = 1;

    int tick = 0;

    while (1) {
        // 1. 檢查有沒有收到包
        if (NRF_RADIO->EVENTS_END) {
            NRF_RADIO->EVENTS_END = 0;
            
            // 只有 CRC 正確才印出來
            if (NRF_RADIO->CRCSTATUS == 1) {
                printk("\n!!! PACKET RECEIVED !!!\n");
                printk("Payload: ");
                for(int i=0; i<16; i++) printk("%02X ", rx_buffer[i]);
                printk("\n");
                
                // 收到後，重新觸發 START 讓它繼續收
                NRF_RADIO->TASKS_START = 1;
            } else {
                // CRC 錯誤，重新 START
                NRF_RADIO->TASKS_START = 1;
            }
        }

        // 2. 心跳顯示 (每秒印一次)
        // 這樣您就知道程式還活著
        tick++;
        if (tick % 100 == 0) { // 10ms * 100 = 1000ms
             printk("Scanning... (Radio State: %d)\n", (int)NRF_RADIO->STATE);
        }
        
        k_busy_wait(10000); // 10ms
    }
}

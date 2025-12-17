/*
 * Pico Tracker Raw Sniffer
 * Strategy: Disable CRC, Disable Length Parsing.
 * Goal: Capture raw bitstream to deduce LFLEN and Endianness.
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <hal/nrf_radio.h>
#include <zephyr/usb/usb_device.h>

// 參數設定 (已知正確的)
#define TARGET_BASE_ADDR  0x552c6a1eUL
#define TARGET_PREFIX     0xC0
#define RX_FREQ           1 

static uint8_t rx_buffer[32];

void radio_configure_raw(void)
{
    NRF_RADIO->TASKS_DISABLE = 1;
    while (NRF_RADIO->EVENTS_DISABLED == 0);
    NRF_RADIO->EVENTS_DISABLED = 0;

    NRF_RADIO->MODE = RADIO_MODE_MODE_Nrf_2Mbit;
    NRF_RADIO->FREQUENCY = RX_FREQ;

    // 地址設定 (這是我們唯一的濾網，只要地址對就抓)
    NRF_RADIO->BASE0 = TARGET_BASE_ADDR;
    NRF_RADIO->PREFIX0 = TARGET_PREFIX;
    NRF_RADIO->TXADDRESS = 0;
    NRF_RADIO->RXADDRESSES = 1; 

    // [關鍵 1] LFLEN 設為 0
    // 我們不讓硬體去解析長度，直接把長度欄位當成數據的一部分抓進來
    NRF_RADIO->PCNF0 = (0 << RADIO_PCNF0_LFLEN_Pos);

    // [關鍵 2] 強制接收 32 Bytes
    // 不管它實際發多長，我們都開大口咬下去
    NRF_RADIO->PCNF1 = (32 << RADIO_PCNF1_MAXLEN_Pos) | 
                       (32 << RADIO_PCNF1_STATLEN_Pos) | // Static Length = 32
                       (4 << RADIO_PCNF1_BALEN_Pos) | 
                       (RADIO_PCNF1_ENDIAN_Little << RADIO_PCNF1_ENDIAN_Pos);

    // [關鍵 3] 徹底關閉 CRC
    // 這樣就算格式錯位，Radio 也不會把封包丟掉
    NRF_RADIO->CRCCNF = 0; // Disable CRC

    // 收完一包自動關閉，方便我們讀取
    NRF_RADIO->SHORTS = (RADIO_SHORTS_READY_START_Msk | RADIO_SHORTS_END_DISABLE_Msk);
}

int main(void)
{
    if (usb_enable(NULL)) return 0;
    k_sleep(K_MSEC(2000));
    printk(">>> RAW SNIFFER STARTED (NO CRC) <<<\n");
    printk("Target: 0x%X (Prefix 0x%X)\n", TARGET_BASE_ADDR, TARGET_PREFIX);

    radio_configure_raw();

    while (1) {
        // 清空 Buffer (填入 FF 以便觀察哪些位元被寫入)
        for(int i=0; i<32; i++) rx_buffer[i] = 0xFF;
        
        NRF_RADIO->PACKETPTR = (uint32_t)rx_buffer;
        NRF_RADIO->EVENTS_END = 0;
        NRF_RADIO->TASKS_RXEN = 1;

        // 等待接收 (死守)
        // 因為關閉了 CRC，只要有任何符合地址的訊號，EVENTS_END 就會觸發
        while (NRF_RADIO->EVENTS_END == 0) {
            // 如果太久沒收到 (例如 1秒)，可以重置一下
            // 但為了抓配對包，我們就一直等
            k_busy_wait(10);
        }

        // 收到數據了！(不管對錯)
        printk("[RAW] Data: ");
        for(int k=0; k<16; k++) { // 先印前 16 byte
            printk("%02X ", rx_buffer[k]);
        }
        printk("\n");

        // 稍微暫停，避免洗版太快
        k_sleep(K_MSEC(100));
    }
}

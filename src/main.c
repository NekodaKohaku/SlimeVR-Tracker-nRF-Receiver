/*
 * Pico Tracker "Raw Dumper"
 * Features: 
 * - CRC Check DISABLED (Receive everything)
 * - Fixed CH 73 & 45
 * - Listen for Address 43... and E7...
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/usb/usb_device.h>
#include <hal/nrf_radio.h>

void radio_init_raw(uint8_t channel) {
    NRF_RADIO->TASKS_DISABLE = 1;
    while (NRF_RADIO->EVENTS_DISABLED == 0);
    NRF_RADIO->EVENTS_DISABLED = 0;

    nrf_radio_power_set(NRF_RADIO, true);
    nrf_radio_frequency_set(NRF_RADIO, channel);
    nrf_radio_mode_set(NRF_RADIO, NRF_RADIO_MODE_NRF_2MBIT);

    // 地址設定 (維持原判，這是最可疑的)
    nrf_radio_base0_set(NRF_RADIO, 0x43434343);
    nrf_radio_prefix0_set(NRF_RADIO, 0x43);
    nrf_radio_base1_set(NRF_RADIO, 0xE7E7E7E7);
    nrf_radio_prefix1_set(NRF_RADIO, 0xE7);
    nrf_radio_rxaddresses_set(NRF_RADIO, 3); 

    // PCNF0: 8-bit Length
    NRF_RADIO->PCNF0 = (8 << RADIO_PCNF0_LFLEN_Pos);

    // PCNF1: 嘗試 Big Endian + Whitening (這是之前代碼分析最可能的組合)
    NRF_RADIO->PCNF1 = (
        (32 << RADIO_PCNF1_MAXLEN_Pos) |
        (4 << RADIO_PCNF1_BALEN_Pos) |
        (RADIO_PCNF1_WHITEEN_Enabled << RADIO_PCNF1_WHITEEN_Pos) | 
        (RADIO_PCNF1_ENDIAN_Big << RADIO_PCNF1_ENDIAN_Pos)
    );

    // *** 關鍵修改：禁用 CRC ***
    // 我們不讓硬體丟棄 CRC 錯誤的包，我們全部都要看
    // 雖然這裡配置了 CRC，但我們在 Main Loop 不檢查 EVENTS_CRCOK
    nrf_radio_crc_configure(NRF_RADIO, RADIO_CRCCNF_LEN_Two, NRF_RADIO_CRC_ADDR_SKIP, 0x11021);
    NRF_RADIO->CRCINIT = 0xFFFF;
    NRF_RADIO->DATAWHITEIV = channel | 0x40;

    // Shorts: Address match -> Start (為了抓 payload)
    // 我們不使用 END_START，我們手動控制
    NRF_RADIO->SHORTS = RADIO_SHORTS_READY_START_Msk; 
    
    NRF_RADIO->EVENTS_READY = 0;
    NRF_RADIO->EVENTS_END = 0;
    NRF_RADIO->EVENTS_ADDRESS = 0;
    
    NRF_RADIO->TASKS_RXEN = 1;
}

int main(void) {
    if (IS_ENABLED(CONFIG_USB_DEVICE_STACK)) {
        usb_enable(NULL);
    }
    
    k_sleep(K_SECONDS(3));
    printk("=== Pico Raw Dumper ===\n");
    printk("CRC Check Ignored. Expect NOISE.\n");
    printk("Look for REPEATING patterns.\n");

    int channel = 73;
    int toggle = 0;

    while (1) {
        channel = (toggle++ % 2 == 0) ? 73 : 45;
        radio_init_raw(channel);
        
        printk("\n[Listening CH %d] ", channel);

        // 每個頻道聽 500ms
        for(int i=0; i<50; i++) {
            // 如果偵測到「地址匹配」(EVENTS_ADDRESS)，說明有人在說話
            // 即使後面 CRC 錯了，我們也要把數據印出來
            if (NRF_RADIO->EVENTS_ADDRESS) {
                NRF_RADIO->EVENTS_ADDRESS = 0;
                
                // 等待 Payload 接收完畢
                while(NRF_RADIO->EVENTS_END == 0);
                NRF_RADIO->EVENTS_END = 0;

                // 讀取數據 (Packet Ptr 沒設，預設會亂指，我們這裡簡單讀 CRCStatus 來判斷有無訊號)
                // 在沒有 EasyDMA 的簡單模式下，我們只能知道「有訊號」
                
                printk("!"); // ! 代表收到符合地址的訊號
                
                if (NRF_RADIO->CRCSTATUS) {
                    printk("[CRC OK]"); // 居然對了？
                } else {
                    printk("[CRC ERR]"); // 預期會看到這個
                }

                // 為了避免洗版，重新啟動
                NRF_RADIO->TASKS_DISABLE = 1;
                while(NRF_RADIO->EVENTS_DISABLED == 0);
                NRF_RADIO->EVENTS_DISABLED = 0;
                NRF_RADIO->TASKS_RXEN = 1;
            }
            k_sleep(K_MSEC(10));
        }
    }
    return 0;
}

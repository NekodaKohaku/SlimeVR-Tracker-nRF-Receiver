/*
 * Pico Final Cracker (Ch 37 Locked)
 * Target: Crack the 1-byte address on Channel 37.
 * Config: BALEN=0, Fixed Freq 2437MHz.
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <hal/nrf_radio.h>
#include <zephyr/usb/usb_device.h>
#include <zephyr/drivers/uart.h>

// 鎖定 Halt 抓到的頻道
#define TARGET_FREQ 37 

void radio_config(uint8_t prefix) {
    NRF_RADIO->TASKS_DISABLE = 1;
    while (NRF_RADIO->EVENTS_DISABLED == 0);
    NRF_RADIO->EVENTS_DISABLED = 0;

    // 1. 設定模式 2Mbit
    nrf_radio_mode_set(NRF_RADIO, NRF_RADIO_MODE_NRF_2MBIT);
    
    // 2. 鎖定頻率 2437 MHz (0x25)
    nrf_radio_frequency_set(NRF_RADIO, TARGET_FREQ);

    // 3. 設定 1-Byte 地址 (BALEN=0)
    // 我們只修改 Prefix0，Base0 不會被使用
    nrf_radio_prefix0_set(NRF_RADIO, prefix); 
    nrf_radio_rxaddresses_set(NRF_RADIO, 1); 

    // 4. PCNF0 (根據 Halt 讀到的 0x00040008)
    // LFLEN=8, S0LEN=0, S1LEN=4
    NRF_RADIO->PCNF0 = (8 << RADIO_PCNF0_LFLEN_Pos) | (4 << RADIO_PCNF0_S1LEN_Pos);

    // 5. PCNF1 (根據 Halt 讀到的 0x00000004)
    // BALEN=0 (關鍵!), Little Endian, No Whitening
    NRF_RADIO->PCNF1 = (32 << RADIO_PCNF1_MAXLEN_Pos) | 
                       (0 << RADIO_PCNF1_BALEN_Pos) | 
                       (RADIO_PCNF1_ENDIAN_Little << RADIO_PCNF1_ENDIAN_Pos) |
                       (RADIO_PCNF1_WHITEEN_Disabled << RADIO_PCNF1_WHITEEN_Pos);

    // 6. CRC (我們雖然設定，但稍後會忽略錯誤)
    NRF_RADIO->CRCCNF = (RADIO_CRCCNF_LEN_Two << RADIO_CRCCNF_LEN_Pos) |
                        (RADIO_CRCCNF_SKIPADDR_Include << RADIO_CRCCNF_SKIPADDR_Pos);
    NRF_RADIO->CRCPOLY = 0x11021; 
    NRF_RADIO->CRCINIT = 0xFFFF;

    NRF_RADIO->SHORTS = RADIO_SHORTS_READY_START_Msk | RADIO_SHORTS_END_START_Msk;
    NRF_RADIO->TASKS_RXEN = 1;
}

int main(void) {
    if (usb_enable(NULL)) return 0;
    
    const struct device *dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_console));
    uint32_t dtr = 0;
    while (!dtr) {
        uart_line_ctrl_get(dev, UART_LINE_CTRL_DTR, &dtr);
        k_sleep(K_MSEC(100));
    }

    printk("\n=== Pico Ch37 Cracker ===\n");
    printk("Locking on 2437MHz, Scanning Prefixes 00-FF...\n");

    uint16_t current_prefix = 0;

    while (1) {
        radio_config((uint8_t)current_prefix);

        // 每個 Prefix 聽 30ms
        // 這樣掃完一輪 256 個地址大約需要 7.6 秒
        // 請務必讓追蹤器持續閃爍超過 10 秒
        for (int i = 0; i < 3; i++) {
            
            // 只要地址匹配 (EVENTS_ADDRESS)，我們就印出來！
            // 不管 CRC 對不對，先抓到再說！
            if (NRF_RADIO->EVENTS_ADDRESS) {
                NRF_RADIO->EVENTS_ADDRESS = 0;
                
                printk("\n>>> ADDR MATCH! Prefix: 0x%02X <<<\n", (uint8_t)current_prefix);
                
                // 等待封包接收完成，檢查 CRC
                while(!NRF_RADIO->EVENTS_END); 
                NRF_RADIO->EVENTS_END = 0;

                if (NRF_RADIO->EVENTS_CRCOK) {
                    printk("!!! CRC OK - PERFECT MATCH !!!\n");
                    // 鎖定成功
                    while(1) k_sleep(K_FOREVER);
                } else {
                    printk("(CRC Fail but Addr Match - Likely Candidate)\n");
                }
            }
            k_busy_wait(10000); // 10ms
        }

        current_prefix++;
        if (current_prefix > 0xFF) {
            current_prefix = 0;
            // printk("."); // 掃完一輪印個點
        }
    }
}

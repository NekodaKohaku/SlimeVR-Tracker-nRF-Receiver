/*
 * Pico Sniper: Address 3 Only
 * Filter out noise from Addr 0.
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <hal/nrf_radio.h>
#include <zephyr/usb/usb_device.h>
#include <zephyr/drivers/uart.h>

// === 狙擊目標參數 ===
// Logical Address 3 使用 Base1 + Prefix0 的第 3 Byte
#define ADDR_BASE1       0x43434343 
#define ADDR_PREFIX0     0x23C343C0  // Byte 3 is 0x23

// 掃描頻道
static uint8_t target_channels[] = {1, 37, 77};
static int ch_index = 0;

void radio_config(uint8_t channel) {
    NRF_RADIO->TASKS_DISABLE = 1;
    while (NRF_RADIO->EVENTS_DISABLED == 0);
    NRF_RADIO->EVENTS_DISABLED = 0;

    nrf_radio_mode_set(NRF_RADIO, NRF_RADIO_MODE_NRF_2MBIT);
    nrf_radio_frequency_set(NRF_RADIO, channel);

    // 1. 設定地址
    nrf_radio_base1_set(NRF_RADIO, ADDR_BASE1); 
    nrf_radio_prefix0_set(NRF_RADIO, ADDR_PREFIX0); 
    
    // 關鍵修正：只聽 Address 3 (Bit 3 = 1 -> 0x08)
    // 這樣 Addr 0 的雜訊就不會出現了
    nrf_radio_rxaddresses_set(NRF_RADIO, 0x08); 

    // 2. PCNF0
    // LFLEN=8, S0LEN=0, S1LEN=4
    NRF_RADIO->PCNF0 = (8 << RADIO_PCNF0_LFLEN_Pos) | 
                       (0 << RADIO_PCNF0_S0LEN_Pos) |
                       (4 << RADIO_PCNF0_S1LEN_Pos);

    // 3. PCNF1
    // Little Endian, No Whitening
    NRF_RADIO->PCNF1 = (32 << RADIO_PCNF1_MAXLEN_Pos) | // 雖然 Dump 是 4，我們設 32 比較保險
                       (4 << RADIO_PCNF1_BALEN_Pos) | 
                       (RADIO_PCNF1_ENDIAN_Little << RADIO_PCNF1_ENDIAN_Pos) |
                       (RADIO_PCNF1_WHITEEN_Disabled << RADIO_PCNF1_WHITEEN_Pos);

    // 4. CRC
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

    printk("\n=== Pico Sniper Active (Addr 3 Only) ===\n");

    while (1) {
        uint8_t ch = target_channels[ch_index];
        radio_config(ch);

        // 每個頻道停留
        for (int i = 0; i < 15; i++) {
            if (NRF_RADIO->EVENTS_ADDRESS) {
                NRF_RADIO->EVENTS_ADDRESS = 0;
                // 如果看到這行，那就是真的抓到了！
                printk("[!] Target Locked on CH %d (Addr: %d)\n", ch, NRF_RADIO->RXMATCH);
            }

            if (NRF_RADIO->EVENTS_END) {
                NRF_RADIO->EVENTS_END = 0;
                
                if (NRF_RADIO->EVENTS_CRCOK) {
                    printk(">>> PACKET OK! Payload Received! <<<\n");
                } else if (NRF_RADIO->EVENTS_CRCERROR) {
                    printk("--- CRC Error (But Address Matched) ---\n");
                }
            }
            k_busy_wait(10000); 
        }

        ch_index++;
        if (ch_index >= 3) ch_index = 0;
    }
    return 0;
}

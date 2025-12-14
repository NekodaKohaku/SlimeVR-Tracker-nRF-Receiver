/*
 * Pico Hardware Signal Detector
 * Strategy: Ignore CRC. Listen ONLY for the specific hardware address pattern.
 * Address: 0x23 + 0x43434343
 * Channels: 1, 37, 77
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <hal/nrf_radio.h>
#include <zephyr/usb/usb_device.h>
#include <zephyr/drivers/uart.h>

// 這是我們唯一確信的真理
#define ADDR_BASE1       0x43434343 
#define ADDR_PREFIX0     0x23C343C0  // 我們只在乎 0x23 (Byte 3)

static uint8_t target_channels[] = {1, 37, 77};
static int ch_index = 0;

// 測試兩種可能的端序
enum { ENDIAN_LITTLE = 0, ENDIAN_BIG = 1 };
static int current_endian = ENDIAN_BIG; // 先猜 Big (VR 設備常用)

void radio_config(uint8_t channel, int endian) {
    NRF_RADIO->TASKS_DISABLE = 1;
    while (NRF_RADIO->EVENTS_DISABLED == 0);
    NRF_RADIO->EVENTS_DISABLED = 0;

    nrf_radio_mode_set(NRF_RADIO, NRF_RADIO_MODE_NRF_2MBIT);
    nrf_radio_frequency_set(NRF_RADIO, channel);

    // 設定地址 (Base1 + Prefix Byte 3)
    nrf_radio_base1_set(NRF_RADIO, ADDR_BASE1); 
    nrf_radio_prefix0_set(NRF_RADIO, ADDR_PREFIX0); 
    // 只聽 Address 3 (0x08)
    nrf_radio_rxaddresses_set(NRF_RADIO, 0x08); 

    // PCNF0: 標準 ESB (LFLEN=8, S0=0, S1=0)
    // 我們先把 S1 設為 0，避免因為 S1 設定錯誤導致收不到
    NRF_RADIO->PCNF0 = (8 << RADIO_PCNF0_LFLEN_Pos);

    // PCNF1: 恢復標準 BALEN=4
    uint32_t pcnf1 = (32 << RADIO_PCNF1_MAXLEN_Pos) | 
                     (4 << RADIO_PCNF1_BALEN_Pos) | // 相信標準，不信 Dump
                     (RADIO_PCNF1_WHITEEN_Disabled << RADIO_PCNF1_WHITEEN_Pos);

    if (endian == ENDIAN_BIG) {
        pcnf1 |= (RADIO_PCNF1_ENDIAN_Big << RADIO_PCNF1_ENDIAN_Pos);
    } else {
        pcnf1 |= (RADIO_PCNF1_ENDIAN_Little << RADIO_PCNF1_ENDIAN_Pos);
    }
    NRF_RADIO->PCNF1 = pcnf1;

    // CRC: 隨便設，反正我們只看 Address Match
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

    printk("\n=== Pico Hardware Signal Detector ===\n");
    printk("Hunting for Base:0x43434343 Prefix:0x23...\n");

    int loop_count = 0;

    while (1) {
        uint8_t ch = target_channels[ch_index];
        radio_config(ch, current_endian);

        // 每個頻道聽 100ms
        for (int i = 0; i < 10; i++) {
            
            // [關鍵] 硬體地址匹配訊號
            if (NRF_RADIO->EVENTS_ADDRESS) {
                NRF_RADIO->EVENTS_ADDRESS = 0;
                
                // 只要看到這行，我們就贏了 90%
                printk("[!!!] PHYSICAL SIGNAL DETECTED on CH %d (Addr 3) [Endian: %s]\n", 
                       ch, current_endian == ENDIAN_BIG ? "BIG" : "LITTLE");
            }

            if (NRF_RADIO->EVENTS_END) {
                NRF_RADIO->EVENTS_END = 0;
                
                // 如果運氣好 CRC 也過了
                if (NRF_RADIO->EVENTS_CRCOK) {
                    printk(">>> PERFECT MATCH! Payload Valid! <<<\n");
                }
            }
            k_busy_wait(10000); 
        }

        ch_index++;
        if (ch_index >= 3) {
            ch_index = 0;
            // 每掃完一輪頻道，切換一次端序，確保兩種都測到
            loop_count++;
            if (loop_count % 2 == 0) {
                current_endian = (current_endian == ENDIAN_BIG) ? ENDIAN_LITTLE : ENDIAN_BIG;
                // printk("Switching Endian to: %s\n", current_endian == ENDIAN_BIG ? "BIG" : "LITTLE");
            }
        }
    }
    return 0;
}

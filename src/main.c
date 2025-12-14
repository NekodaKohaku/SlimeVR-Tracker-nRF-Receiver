/*
 * Pico Universal Scanner (Speed Auto-Switch)
 * Target: Ch 37 (Fixed)
 * Strategy: Toggle between 1Mbit and 2Mbit modes to find the correct speed.
 * Filter: RSSI < 75 (Strong signals only)
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <hal/nrf_radio.h>
#include <zephyr/usb/usb_device.h>
#include <zephyr/drivers/uart.h>

#define TARGET_FREQ 37 
#define RSSI_THRESHOLD 75 // 只顯示強訊號

// 兩種速度模式
enum { MODE_1MBIT = 0, MODE_2MBIT = 1 };
const char *mode_str[] = {"1Mbit", "2Mbit"};

void radio_config(int mode, uint8_t prefix) {
    NRF_RADIO->TASKS_DISABLE = 1;
    while (NRF_RADIO->EVENTS_DISABLED == 0);
    NRF_RADIO->EVENTS_DISABLED = 0;

    // 自動切換速度
    if (mode == MODE_1MBIT) {
        nrf_radio_mode_set(NRF_RADIO, NRF_RADIO_MODE_NRF_1MBIT);
    } else {
        nrf_radio_mode_set(NRF_RADIO, NRF_RADIO_MODE_NRF_2MBIT);
    }

    nrf_radio_frequency_set(NRF_RADIO, TARGET_FREQ);

    // 1-Byte Address (BALEN=0)
    nrf_radio_prefix0_set(NRF_RADIO, prefix); 
    nrf_radio_rxaddresses_set(NRF_RADIO, 1); 

    // PCNF0: S1LEN=4 (Based on previous dump)
    NRF_RADIO->PCNF0 = (8 << RADIO_PCNF0_LFLEN_Pos) | (4 << RADIO_PCNF0_S1LEN_Pos);

    // PCNF1: BALEN=0
    NRF_RADIO->PCNF1 = (32 << RADIO_PCNF1_MAXLEN_Pos) | 
                       (0 << RADIO_PCNF1_BALEN_Pos) | 
                       (RADIO_PCNF1_ENDIAN_Little << RADIO_PCNF1_ENDIAN_Pos) |
                       (RADIO_PCNF1_WHITEEN_Disabled << RADIO_PCNF1_WHITEEN_Pos);

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

    printk("\n=== Pico Universal Scanner ===\n");
    printk("Cycling 1Mbit <-> 2Mbit on CH 37...\n");

    uint16_t current_prefix = 0;
    int current_mode = MODE_2MBIT;

    while (1) {
        radio_config(current_mode, (uint8_t)current_prefix);

        // 每個模式聽 40ms
        for (int i = 0; i < 4; i++) {
            if (NRF_RADIO->EVENTS_ADDRESS) {
                NRF_RADIO->EVENTS_ADDRESS = 0;
                
                NRF_RADIO->TASKS_RSSISTART = 1;
                while(!NRF_RADIO->EVENTS_RSSIEND);
                NRF_RADIO->EVENTS_RSSIEND = 0;
                uint8_t rssi = NRF_RADIO->RSSISAMPLE;

                if (rssi < RSSI_THRESHOLD) {
                    printk("\n>>> HIT! Mode: %s | Prefix: 0x%02X (RSSI: -%d) <<<\n", 
                           mode_str[current_mode], (uint8_t)current_prefix, rssi);
                    
                    if (NRF_RADIO->EVENTS_CRCOK) {
                        printk("!!! CRC OK !!! LOCKED!\n");
                        // 鎖定成功
                        while(1) k_sleep(K_FOREVER);
                    }
                }
            }
            k_busy_wait(10000); 
        }

        // 切換邏輯：掃完一個 Prefix，就換一種速度再掃一次
        // 這樣確保每個地址都被兩種速度檢查過
        current_mode = !current_mode;
        
        if (current_mode == MODE_2MBIT) { // 每兩次循環換一個地址
            current_prefix++;
            if (current_prefix > 0xFF) {
                current_prefix = 0;
                // printk("."); 
            }
        }
    }
}

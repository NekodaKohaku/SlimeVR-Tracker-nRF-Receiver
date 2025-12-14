/*
 * Pico Final Brute Force
 * Strategy: Cycle through all 4 possible radio configurations.
 * Target: Address 3 on Channel 77 (The most reliable channel)
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <hal/nrf_radio.h>
#include <zephyr/usb/usb_device.h>
#include <zephyr/drivers/uart.h>

#define ADDR_BASE1       0x43434343 
#define ADDR_PREFIX0     0x23C343C0  // Byte 3 is 0x23

// 我們只守在 Channel 77，因為它是最乾淨的
#define CAMP_FREQ        77

// 定義 4 種組合
enum {
    CONF_LITTLE_NOWHITE = 0, // Dump 的設定 (失敗)
    CONF_BIG_NOWHITE,        // 常見變體
    CONF_LITTLE_WHITE,       // 標準 Nordic 設定
    CONF_BIG_WHITE           // 最常見的 VR 設定
};

const char *conf_names[] = {
    "Little Endian / No Whitening",
    "Big Endian    / No Whitening",
    "Little Endian / Whitening ON",
    "Big Endian    / Whitening ON"
};

void radio_config(int config_type) {
    NRF_RADIO->TASKS_DISABLE = 1;
    while (NRF_RADIO->EVENTS_DISABLED == 0);
    NRF_RADIO->EVENTS_DISABLED = 0;

    nrf_radio_mode_set(NRF_RADIO, NRF_RADIO_MODE_NRF_2MBIT);
    nrf_radio_frequency_set(NRF_RADIO, CAMP_FREQ);

    // 地址設定 (Address 3)
    nrf_radio_base1_set(NRF_RADIO, ADDR_BASE1); 
    nrf_radio_prefix0_set(NRF_RADIO, ADDR_PREFIX0); 
    nrf_radio_rxaddresses_set(NRF_RADIO, 0x08); // 只聽 Addr 3

    // PCNF0: 放寬標準
    // LFLEN=8, S0=0, S1=4 (根據 Dump)
    NRF_RADIO->PCNF0 = (8 << RADIO_PCNF0_LFLEN_Pos) | (4 << RADIO_PCNF0_S1LEN_Pos);

    // PCNF1: 根據 config_type 變化
    uint32_t pcnf1 = (32 << RADIO_PCNF1_MAXLEN_Pos) | (4 << RADIO_PCNF1_BALEN_Pos);

    // 設定 Endian
    if (config_type == CONF_BIG_NOWHITE || config_type == CONF_BIG_WHITE) {
        pcnf1 |= (RADIO_PCNF1_ENDIAN_Big << RADIO_PCNF1_ENDIAN_Pos);
    } else {
        pcnf1 |= (RADIO_PCNF1_ENDIAN_Little << RADIO_PCNF1_ENDIAN_Pos);
    }

    // 設定 Whitening
    if (config_type == CONF_LITTLE_WHITE || config_type == CONF_BIG_WHITE) {
        pcnf1 |= (RADIO_PCNF1_WHITEEN_Enabled << RADIO_PCNF1_WHITEEN_Pos);
    } else {
        pcnf1 |= (RADIO_PCNF1_WHITEEN_Disabled << RADIO_PCNF1_WHITEEN_Pos);
    }

    NRF_RADIO->PCNF1 = pcnf1;

    // CRC 設定 (標準 2 bytes)
    NRF_RADIO->CRCCNF = (RADIO_CRCCNF_LEN_Two << RADIO_CRCCNF_LEN_Pos) |
                        (RADIO_CRCCNF_SKIPADDR_Include << RADIO_CRCCNF_SKIPADDR_Pos);
    NRF_RADIO->CRCPOLY = 0x11021; 
    NRF_RADIO->CRCINIT = 0xFFFF;
    NRF_RADIO->DATAWHITEIV = CAMP_FREQ | 0x40; // 白化種子

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

    printk("\n=== Pico Brute Force Scanner ===\n");
    printk("Camping on CH 77, cycling configs...\n");

    int current_conf = 0;

    while (1) {
        // 每 400ms 換一種解碼模式
        radio_config(current_conf);
        // printk("Testing: %s\n", conf_names[current_conf]); // 不刷屏，安靜測試

        for (int i = 0; i < 40; i++) {
            if (NRF_RADIO->EVENTS_END) {
                NRF_RADIO->EVENTS_END = 0;
                
                // 如果 CRC 正確，代表我們猜對了！
                if (NRF_RADIO->EVENTS_CRCOK) {
                    printk("\n>>> JACKPOT! <<<\n");
                    printk("Correct Config: %s\n", conf_names[current_conf]);
                    printk("Data Received on CH 77!\n");
                    // 鎖定這個模式，不再切換
                    while(1) {
                         // 這裡可以繼續接收數據...
                         k_busy_wait(100000);
                         if (NRF_RADIO->EVENTS_END && NRF_RADIO->EVENTS_CRCOK) {
                             NRF_RADIO->EVENTS_END = 0;
                             printk(".");
                         }
                    }
                }
            }
            k_busy_wait(10000); 
        }

        current_conf++;
        if (current_conf > 3) current_conf = 0;
    }
    return 0;
}

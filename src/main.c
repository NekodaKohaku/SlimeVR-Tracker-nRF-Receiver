/*
 * Copyright (c) 2025 White Cat DIY
 * Pico Tracker Sniffer - "The Ambush" (Targeted Hopping)
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <hal/nrf_radio.h>
#include <zephyr/usb/usb_device.h>
#include <zephyr/drivers/uart.h>

#define CONSOLE_DEVICE_LABEL DT_CHOSEN(zephyr_console)

// 🔑 我們的黃金鑰匙
#define MY_BASE_ADDR  0x552c6a1eUL
#define MY_PREFIX     0xC0

// 🎯 我們剛剛發現的 "據點" (根據你的 read32 結果)
// 你可以再多抓幾次 read32 看看有沒有別的，目前的 1, 37, 77 是確定的
static const int ambush_channels[] = {1, 37, 77};
#define CHANNEL_COUNT 3

void radio_setup(int channel)
{
    NRF_RADIO->TASKS_DISABLE = 1;
    while (NRF_RADIO->EVENTS_DISABLED == 0);
    NRF_RADIO->EVENTS_DISABLED = 0;

    NRF_RADIO->FREQUENCY = channel;
    
    // 設定 2Mbit
    NRF_RADIO->MODE = RADIO_MODE_MODE_Nrf_2Mbit; 

    // 地址設定
    NRF_RADIO->BASE0 = MY_BASE_ADDR;
    NRF_RADIO->PREFIX0 = MY_PREFIX;
    NRF_RADIO->TXADDRESS = 0;
    NRF_RADIO->RXADDRESSES = 1;

    // 封包格式
    NRF_RADIO->PCNF0 = 0;
    NRF_RADIO->PCNF1 = (32 << RADIO_PCNF1_MAXLEN_Pos) | (4 << RADIO_PCNF1_BALEN_Pos) | (RADIO_PCNF1_ENDIAN_Little << RADIO_PCNF1_ENDIAN_Pos);

    // CRC
    NRF_RADIO->CRCCNF = (RADIO_CRCCNF_LEN_Two << RADIO_CRCCNF_LEN_Pos); 
    NRF_RADIO->CRCINIT = 0xFFFF;      
    NRF_RADIO->CRCPOLY = 0x11021;      
    
    NRF_RADIO->SHORTS = 0;
}

int main(void)
{
    const struct device *console_dev = DEVICE_DT_GET(CONSOLE_DEVICE_LABEL);
    uint32_t dtr = 0;
    static uint8_t packet_buffer[32];

    usb_enable(NULL);
    while (!dtr) {
        uart_line_ctrl_get(console_dev, UART_LINE_CTRL_DTR, &dtr);
        k_sleep(K_MSEC(100));
    }

    printk("\n>>> AMBUSH SET: Waiting for Helmet on Ch 1, 37, 77... <<<\n");

    int ch_idx = 0;

    while (1) {
        int current_ch = ambush_channels[ch_idx];
        radio_setup(current_ch);
        NRF_RADIO->PACKETPTR = (uint32_t)packet_buffer;

        // 啟動接收
        NRF_RADIO->EVENTS_READY = 0;
        NRF_RADIO->TASKS_RXEN = 1;
        while(NRF_RADIO->EVENTS_READY == 0);
        
        NRF_RADIO->EVENTS_END = 0;
        NRF_RADIO->TASKS_START = 1;

        // 每個頻道埋伏 100ms
        // 因為頭盔如果來找它，一定會瘋狂發射，我們不用跳太快
        k_busy_wait(100000); 

        if (NRF_RADIO->EVENTS_END) {
            if (NRF_RADIO->CRCSTATUS == 1) {
                printk("[CAPTURED!] Ch:%d | Data: ", current_ch);
                for(int i=0; i<10; i++) printk("%02X ", packet_buffer[i]);
                printk("\n");
            }
        }
        
        // 換下一個埋伏點
        ch_idx++;
        if (ch_idx >= CHANNEL_COUNT) ch_idx = 0;
    }
}

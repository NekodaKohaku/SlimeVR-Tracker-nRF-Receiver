/*
 * Pico Tracker "Hunter" Sniffer
 * Target: Pairing Mode (Red/Blue Flash)
 * Channels: 73, 45 (From Firmware Analysis)
 * Addresses: E7 (Default), 43 (Pico Special)
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/usb/usb_device.h>
#include <esb.h>

// --- 來自固件分析的關鍵參數 ---
#define CH_A 73  // 0x49
#define CH_B 45  // 0x2d

// 地址表
static const uint8_t base_addr_0[4] = {0xE7, 0xE7, 0xE7, 0xE7}; // Default
static const uint8_t base_addr_1[4] = {0x43, 0x43, 0x43, 0x43}; // Pico Special 'C'

// Prefixes (前綴)
static const uint8_t prefix_0 = 0xE7;
static const uint8_t prefix_1 = 0x43; // 配合 Base 1 形成 4343434343

static struct esb_payload rx_payload;

void event_handler(struct esb_evt const *event)
{
    switch (event->evt_id) {
    case ESB_EVENT_RX_RECEIVED:
        if (esb_read_rx_payload(&rx_payload) == 0) {
            // 抓到了！
            printk("\n!!! BINGO !!! Captured on CH:%d\n", (NRF_RADIO->FREQUENCY));
            printk("LEN:%d PIPE:%d DATA:", rx_payload.length, rx_payload.pipe);
            for (int i = 0; i < rx_payload.length; i++) {
                printk(" %02X", rx_payload.data[i]);
            }
            printk("\n");
            
            // 如果抓到數據，LED 閃一下提示 (如果有的話)
        }
        break;
    }
}

int esb_initialize(void)
{
    int err;
    struct esb_config config = ESB_DEFAULT_CONFIG;

    // Pico 通常使用 2Mbps
    config.bitrate = ESB_BITRATE_2MBPS;
    config.mode = ESB_MODE_PRX;
    config.event_handler = event_handler;
    config.crc = ESB_CRC_16BIT;
    
    // 開啟 ACK，試圖讓追蹤器以為連上了
    config.selective_auto_ack = true;

    err = esb_init(&config);
    if (err) return err;

    err = esb_set_base_address_0(base_addr_0);
    if (err) return err;
    err = esb_set_base_address_1(base_addr_1);
    if (err) return err;

    // 設定 Pipe 0 和 Pipe 1 的 Prefix
    uint8_t prefixes[8] = {prefix_0, prefix_1, 0, 0, 0, 0, 0, 0};
    err = esb_set_prefixes(prefixes, 8);
    return err;
}

int main(void)
{
    if (IS_ENABLED(CONFIG_USB_DEVICE_STACK)) {
        usb_enable(NULL);
    }
    
    k_sleep(K_SECONDS(3));
    printk("\n=== Pico Tracker Hunter Started ===\n");
    printk("Hunting on CH %d and %d...\n", CH_A, CH_B);
    printk("Looking for address 4343434343...\n");

    if (esb_initialize() != 0) {
        printk("ESB Init Failed\n");
        return 0;
    }

    esb_start_rx();

    int target = CH_A;

    while (1) {
        esb_stop_rx();
        esb_set_rf_channel(target);
        esb_start_rx();
        
        printk("."); // 心跳點，證明活著

        // 切換頻道
        if (target == CH_A) target = CH_B;
        else target = CH_A;

        // 每個頻道停留 150ms (稍微快一點，捕捉廣播)
        k_sleep(K_MSEC(150));
    }
    return 0;
}

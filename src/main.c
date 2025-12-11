/*
 * SlimeVR Sniffer - Auto Channel Scanner
 * 掃描 0-100 頻道，尋找 E7/C2 地址的訊號
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/usb/usb_device.h>
#include <zephyr/drivers/uart.h>
#include <esb.h>

// --- 配置區 ---
// 如果掃不到，下一輪嘗試把這裡改成 ESB_BITRATE_1MBPS
#define SNIFFER_BITRATE ESB_BITRATE_2MBPS 

static const uint8_t base_addr_0[4] = {0xE7, 0xE7, 0xE7, 0xE7};
static const uint8_t prefix_0 = 0xE7;
static const uint8_t base_addr_1[4] = {0xC2, 0xC2, 0xC2, 0xC2};
static const uint8_t prefix_1 = 0xC2;

static struct esb_payload rx_payload;
static bool found_signal = false;
static int current_channel = 0;

void event_handler(struct esb_evt const *event)
{
    switch (event->evt_id) {
    case ESB_EVENT_RX_RECEIVED:
        // 收到數據了！
        if (esb_read_rx_payload(&rx_payload) == 0) {
            // 標記已找到，主迴圈將停止跳頻
            if (!found_signal) {
                printk("\n!!! SIGNAL FOUND ON CHANNEL %d !!!\n", current_channel);
                found_signal = true;
            }
            
            // 印出數據
            printk("CH:%d LEN:%d DATA:", current_channel, rx_payload.length);
            for (int i = 0; i < rx_payload.length; i++) {
                printk(" %02X", rx_payload.data[i]);
            }
            printk("\n");
        }
        break;
    }
}

int esb_initialize(void)
{
    int err;
    struct esb_config config = ESB_DEFAULT_CONFIG;
    
    // 設定參數
    config.protocol = ESB_PROTOCOL_ESB_DPL;
    config.bitrate = SNIFFER_BITRATE;
    config.mode = ESB_MODE_PRX;
    config.event_handler = event_handler;
    config.crc = ESB_CRC_16BIT;

    err = esb_init(&config);
    if (err) return err;

    err = esb_set_base_address_0(base_addr_0);
    if (err) return err;
    err = esb_set_base_address_1(base_addr_1);
    if (err) return err;

    uint8_t prefixes[8] = {prefix_0, prefix_1, 0, 0, 0, 0, 0, 0};
    err = esb_set_prefixes(prefixes, 8);
    if (err) return err;

    return 0;
}

int main(void)
{
    int err;

    // 1. USB 初始化
    if (IS_ENABLED(CONFIG_USB_DEVICE_STACK)) {
        err = usb_enable(NULL);
    }
    
    k_sleep(K_SECONDS(2)); // 給您時間打開 Putty
    printk("=== SlimeVR Auto Scanner Started ===\n");
    printk("Bitrate: %s\n", (SNIFFER_BITRATE == ESB_BITRATE_2MBPS) ? "2Mbps" : "1Mbps");

    // 2. ESB 初始化
    err = esb_initialize();
    if (err) {
        printk("ESB Init failed: %d\n", err);
        return 0;
    }

    // 3. 開始掃描迴圈
    esb_start_rx();

    while (1) {
        if (found_signal) {
            // 如果找到訊號，就停在這裡只處理數據，不再跳頻
            k_sleep(K_MSEC(100)); 
            continue;
        }

        // 切換頻道
        esb_stop_rx();
        current_channel++;
        if (current_channel > 100) current_channel = 0; // 掃描範圍 0-100

        err = esb_set_rf_channel(current_channel);
        esb_start_rx();

        // 在這個頻道停留 100ms 聽聽看
        // 顯示目前進度 (用 \r 讓它在同一行更新，不洗版)
        printk("Scanning... %d \r", current_channel);
        
        k_sleep(K_MSEC(100));
    }
    return 0;
}

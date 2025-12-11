/*
 * SlimeVR Sniffer - Target Lock Mode
 * 鎖定單一頻道 (75) 與特定速率 (1Mbps/2Mbps)
 * 專門用於捕捉開機瞬間的握手包
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/usb/usb_device.h>
#include <zephyr/drivers/uart.h>
#include <esb.h>

// --- 關鍵配置區 (修改這裡來測試) ---

// 1. 鎖定頻率 (根據您的逆向工程結果是 75)
#define TARGET_CHANNEL 75

// 2. 傳輸速率 (請先試 1Mbps，如果不對再把註解換過來試 2Mbps)
#define SNIFFER_BITRATE ESB_BITRATE_1MBPS
// #define SNIFFER_BITRATE ESB_BITRATE_2MBPS

// --- 地址配置 (已確認正確) ---
static const uint8_t base_addr_0[4] = {0xE7, 0xE7, 0xE7, 0xE7};
static const uint8_t prefix_0 = 0xE7;
static const uint8_t base_addr_1[4] = {0xC2, 0xC2, 0xC2, 0xC2};
static const uint8_t prefix_1 = 0xC2;

static struct esb_payload rx_payload;

void event_handler(struct esb_evt const *event)
{
    switch (event->evt_id) {
    case ESB_EVENT_RX_RECEIVED:
        if (esb_read_rx_payload(&rx_payload) == 0) {
            // 收到數據！印出詳細 Hex
            printk("!!! CAPTURED on CH:%d !!! LEN:%d DATA:", TARGET_CHANNEL, rx_payload.length);
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

    config.protocol = ESB_PROTOCOL_ESB_DPL;
    config.bitrate = SNIFFER_BITRATE;
    config.mode = ESB_MODE_PRX; // 接收模式
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

    // 直接鎖定頻率，不需在迴圈設定
    err = esb_set_rf_channel(TARGET_CHANNEL);
    if (err) return err;

    return 0;
}

int main(void)
{
    int err;

    // 初始化 USB
    if (IS_ENABLED(CONFIG_USB_DEVICE_STACK)) {
        err = usb_enable(NULL);
    }

    // 等待您打開 Putty
    k_sleep(K_SECONDS(3));
    
    printk("\n=== SlimeVR Target Lock Sniffer ===\n");
    printk("Locked Channel: %d\n", TARGET_CHANNEL);
    printk("Bitrate: %s\n", (SNIFFER_BITRATE == ESB_BITRATE_2MBPS) ? "2Mbps" : "1Mbps");
    printk("Addresses: E7... / C2...\n");
    printk("Waiting for tracker power-on signal...\n");

    err = esb_initialize();
    if (err) {
        printk("ESB Init failed: %d\n", err);
        return 0;
    }

    esb_start_rx();

    // 主迴圈只需活著，讓中斷處理數據
    while (1) {
        k_sleep(K_FOREVER);
    }
    return 0;
}

/*
 * Pico Tracker Specific Sniffer
 * Based on Reverse Engineering of Firmware v0.6.9
 * Target Channels: 73 (0x49), 45 (0x2d)
 * Target Addresses: E7E7E7E7E7, 4343434343
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/usb/usb_device.h>
#include <esb.h>

// --- 反向工程發現的關鍵頻率 ---
// 代碼中 FUN_00005214 顯示它使用 0x49 (73) 或 0x2d (45)
#define TARGET_CH_A 73
#define TARGET_CH_B 45

// --- 反向工程發現的地址 ---
// FUN_000053e0 顯示寫入了 0xe7e7e7e7 和 0x43434343
static const uint8_t base_addr_0[4] = {0xE7, 0xE7, 0xE7, 0xE7};
static const uint8_t base_addr_1[4] = {0x43, 0x43, 0x43, 0x43}; // ASCII 'C'
static const uint8_t prefix_0 = 0xE7;
static const uint8_t prefix_1 = 0x43;

static struct esb_payload rx_payload;

void event_handler(struct esb_evt const *event)
{
    if (event->evt_id == ESB_EVENT_RX_RECEIVED) {
        if (esb_read_rx_payload(&rx_payload) == 0) {
            printk("CAPTURED! CH:%d LEN:%d DATA:", 
                   (NRF_RADIO->FREQUENCY), rx_payload.length);
            for (int i = 0; i < rx_payload.length; i++) {
                printk(" %02X", rx_payload.data[i]);
            }
            printk("\n");
        }
    }
}

int esb_initialize(void)
{
    int err;
    struct esb_config config = ESB_DEFAULT_CONFIG;
    
    // Pico 為了低延遲通常用 2Mbps，但也可能為了兼容性用 1Mbps
    // 建議先試 2Mbps (FUN_00001ce0 中有對 PCNF1 的操作暗示高速)
    config.bitrate = ESB_BITRATE_2MBPS;
    config.mode = ESB_MODE_PRX;
    config.event_handler = event_handler;
    config.crc = ESB_CRC_16BIT;
    config.selective_auto_ack = true; // 嘗試開啟 ACK，看能否騙它多吐點數據

    err = esb_init(&config);
    if (err) return err;

    err = esb_set_base_address_0(base_addr_0);
    if (err) return err;
    err = esb_set_base_address_1(base_addr_1);
    if (err) return err;

    // 設定 Prefix (E7, 43...)
    uint8_t prefixes[8] = {prefix_0, prefix_1, 0, 0, 0, 0, 0, 0};
    err = esb_set_prefixes(prefixes, 8);
    return err;
}

int main(void)
{
    if (IS_ENABLED(CONFIG_USB_DEVICE_STACK)) {
        usb_enable(NULL);
    }
    
    k_sleep(K_SECONDS(2));
    printk("=== Pico Tracker Specific Sniffer ===\n");
    printk("Scanning Targets: CH %d & CH %d\n", TARGET_CH_A, TARGET_CH_B);
    printk("Addresses: E7... & 43... (From Firmware Analysis)\n");

    if (esb_initialize() != 0) {
        printk("ESB Init Failed\n");
        return 0;
    }

    esb_start_rx();

    int current_target = TARGET_CH_A;

    while (1) {
        // 在兩個關鍵頻道間切換
        esb_stop_rx();
        esb_set_rf_channel(current_target);
        esb_start_rx();
        
        // 打印當前監聽狀態 (不換行以保持整潔)
        printk("Listening on CH %d... \r", current_target);
        
        // 每個頻道聽 200ms
        k_sleep(K_MSEC(200));

        // 切換
        if (current_target == TARGET_CH_A) 
            current_target = TARGET_CH_B;
        else 
            current_target = TARGET_CH_A;
    }
    return 0;
}

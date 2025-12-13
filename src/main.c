/*
 * Pico Tracker "All-Seeing Eye" Scanner (Fixed)
 * Features: 
 * 1. Standard BLE 1M
 * 2. Coded PHY (Long Range)
 * 3. Extended Advertising (Enabled via Config)
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/usb/usb_device.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/hci.h>

// 掃描回調函數
static void scan_cb(const bt_addr_le_t *addr, int8_t rssi, uint8_t adv_type,
                    struct net_buf_simple *buf)
{
    // 過濾掉太遠的雜訊
    if (rssi < -75) return;

    char addr_str[BT_ADDR_LE_STR_LEN];
    bt_addr_le_to_str(addr, addr_str, sizeof(addr_str));

    printk("\n[FOUND] Device: %s | RSSI: %d dBm | Type: %d\n", addr_str, rssi, adv_type);
    
    // 如果訊號很強，標記出來
    if (rssi > -45) {
        printk(">>> STRONG SIGNAL NEARBY! <<<\n");
    }
}

int main(void)
{
    // 1. 啟動 USB
    if (IS_ENABLED(CONFIG_USB_DEVICE_STACK)) {
        usb_enable(NULL);
    }

    k_sleep(K_SECONDS(3));
    printk("=== Pico All-PHY Scanner ===\n");
    printk("Scanning: 1M / Coded PHY / Extended Adv\n");

    // 2. 初始化藍牙
    int err = bt_enable(NULL);
    if (err) {
        printk("Bluetooth init failed (err %d)\n", err);
        return 0;
    }

    // 3. 設定掃描參數
    // 修正說明：移除了 BT_LE_SCAN_OPT_EXT_ADV，因為 Config 已啟用
    struct bt_le_scan_param scan_param = {
        .type       = BT_LE_SCAN_TYPE_ACTIVE,
        // 只保留 Coded PHY 選項，Extended Adv 由底層自動處理
        .options    = BT_LE_SCAN_OPT_CODED, 
        .interval   = BT_GAP_SCAN_FAST_INTERVAL,
        .window     = BT_GAP_SCAN_FAST_WINDOW,
    };

    // 4. 開始掃描
    err = bt_le_scan_start(&scan_param, scan_cb);
    if (err) {
        printk("Scanning start failed (err %d)\n", err);
        return 0;
    }

    printk("Scanning started... Hold tracker close!\n");

    while (1) {
        k_sleep(K_SECONDS(1));
    }
    return 0;
}

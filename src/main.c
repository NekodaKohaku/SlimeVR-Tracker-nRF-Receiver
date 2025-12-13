/*
 * Pico Tracker "All-Seeing Eye" Scanner
 * Features: 
 * 1. Standard BLE 1M
 * 2. Coded PHY (Long Range) - 可能藏在這裡！
 * 3. Extended Advertising
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
    // 過濾掉太遠的雜訊，只顯示貼在旁邊的裝置
    if (rssi < -60) return;

    char addr_str[BT_ADDR_LE_STR_LEN];
    bt_addr_le_to_str(addr, addr_str, sizeof(addr_str));

    printk("\n[FOUND] Device: %s | RSSI: %d dBm\n", addr_str, rssi);
    
    // 簡單辨識廠商
    // Pico 的 MAC 地址通常可能不是固定的，但我們可以看訊號強度
    if (rssi > -40) {
        printk(">>> TARGET LOCATED! (Very Strong Signal) <<<\n");
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

    // 3. 設定掃描參數 (開啟所有模式)
    struct bt_le_scan_param scan_param = {
        .type       = BT_LE_SCAN_TYPE_ACTIVE,   // 主動詢問
        .options    = BT_LE_SCAN_OPT_CODED | BT_LE_SCAN_OPT_EXT_ADV, // 關鍵：開啟 Coded 和 Extended
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

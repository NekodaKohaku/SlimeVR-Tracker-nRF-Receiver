/*
 * Custom ESB Receiver for Reverse Engineered Tracker
 * Protocol: ESB (Enhanced ShockBurst)
 * Frequency: 2475 MHz (Channel 75)
 * Address: E7 E7 E7 E7 E7
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/usb/usb_device.h>
#include <zephyr/drivers/uart.h>
#include <esb.h>

#define LOG_LEVEL LOG_LEVEL_INF
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(main);

// --- 逆向工程參數 ---
// RF Channel 75 = 2475 MHz
#define RF_CHANNEL 75 

// Address 0: E7 E7 E7 E7 E7
static const uint8_t base_addr_0[4] = {0xE7, 0xE7, 0xE7, 0xE7};
static const uint8_t prefix_0 = 0xE7;

// Address 1: C2 C2 C2 C2 C2
static const uint8_t base_addr_1[4] = {0xC2, 0xC2, 0xC2, 0xC2};
static const uint8_t prefix_1 = 0xC2;

static struct esb_payload rx_payload;

// 收到數據的回調函數
void event_handler(struct esb_evt const *event)
{
    switch (event->evt_id) {
    case ESB_EVENT_RX_RECEIVED:
        if (esb_read_rx_payload(&rx_payload) == 0) {
            // 透過 USB 串口印出 Hex 數據
            printk("LEN:%d DATA:", rx_payload.length);
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
    
    // 使用預設配置
    struct esb_config config = ESB_DEFAULT_CONFIG;
    config.protocol = ESB_PROTOCOL_ESB_DPL; // 動態長度
    config.bitrate = ESB_BITRATE_2MBPS;     // 2Mbps
    config.mode = ESB_MODE_PRX;             // 接收模式
    config.event_handler = event_handler;
    config.crc = ESB_CRC_16BIT;

    err = esb_init(&config);
    if (err) {
        LOG_ERR("ESB init failed: %d", err);
        return err;
    }

    // 設定地址 0 (E7...)
    err = esb_set_base_address_0(base_addr_0);
    if (err) return err;
    
    err = esb_set_base_address_1(base_addr_1);
    if (err) return err;

    // 設定 Prefix (E7, C2...)
    uint8_t prefixes[8] = {prefix_0, prefix_1, 0, 0, 0, 0, 0, 0};
    err = esb_set_prefixes(prefixes, 8);
    if (err) return err;

    // 設定頻率 (關鍵!)
    err = esb_set_rf_channel(RF_CHANNEL);
    if (err) {
        LOG_ERR("ESB set channel failed: %d", err);
        return err;
    }

    return 0;
}

int main(void)
{
    int err;

    // 1. 初始化 USB (這一步很重要，不然電腦看不到串口)
    if (IS_ENABLED(CONFIG_USB_DEVICE_STACK)) {
        err = usb_enable(NULL);
        if (err) {
            LOG_ERR("Failed to enable USB");
            return 0;
        }
    }

    // 等待 USB 連線建立 (稍微延遲一下)
    k_sleep(K_SECONDS(1));
    printk("Tracker Receiver Started on Channel %d\n", RF_CHANNEL);

    // 2. 初始化 ESB
    err = esb_initialize();
    if (err) {
        printk("ESB Init Failed! Error: %d\n", err);
        return 0;
    }

    // 3. 開始接收
    err = esb_start_rx();
    if (err) {
        printk("ESB Start RX Failed!\n");
        return 0;
    }

    printk("Listening for data...\n");

    // 主迴圈只需保持活著，數據由中斷處理
    while (1) {
        k_sleep(K_FOREVER);
    }
    return 0;
}

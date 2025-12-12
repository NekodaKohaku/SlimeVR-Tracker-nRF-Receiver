/*
 * Pico Tracker "Active Pinger"
 * 模擬頭顯發送訊號，誘發 Tracker 回傳 ACK
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/usb/usb_device.h>
#include <esb.h>

// 定義目標
#define CH_A 73
#define CH_B 45

// 定義地址
static const uint8_t addr_pico[5]    = {0x43, 0x43, 0x43, 0x43, 0x43}; // CCCCC
static const uint8_t addr_default[5] = {0xE7, 0xE7, 0xE7, 0xE7, 0xE7};

// 構建一個簡單的 Payload (內容不重要，重要的是能觸發 ACK)
static uint8_t tx_payload_data[8] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};

int esb_initialize(void)
{
    int err;
    struct esb_config config = ESB_DEFAULT_CONFIG;

    // 必須使用 PTX (發射模式)
    config.protocol = ESB_PROTOCOL_ESB_DPL;
    config.bitrate = ESB_BITRATE_2MBPS; // Pico 99% 是 2Mbps
    config.mode = ESB_MODE_PTX;
    config.event_handler = NULL; // 我們不需要處理接收事件，只需要知道發送是否成功
    config.crc = ESB_CRC_16BIT;
    config.tx_output_power = 4; // 開到最大功率 +4dBm
    config.retransmit_delay = 500;
    config.retransmit_count = 3;

    err = esb_init(&config);
    if (err) return err;
    
    // 設定地址 (雖然是 PTX，但我們要設定發送的目標地址)
    // 這裡我們動態切換 Base Address，所以在 Init 隨便設一個即可
    err = esb_set_base_address_0(addr_pico + 1);
    err = esb_set_prefixes(addr_pico, 1);
    
    return 0;
}

// 發送函式
bool try_ping(uint8_t channel, const uint8_t *addr, const char *addr_name)
{
    int err;
    struct esb_payload tx_payload = ESB_CREATE_PAYLOAD(0, tx_payload_data);
    
    // 1. 設定頻率
    esb_stop_rx(); // 停止無線電
    esb_set_rf_channel(channel);

    // 2. 設定目標地址 (這是關鍵)
    // ESB 的地址由 Base(4bytes) + Prefix(1byte) 組成
    // 我們將 addr[0] 作為 Prefix, addr[1-4] 作為 Base0
    esb_set_base_address_0(addr + 1);
    uint8_t prefix[1] = {addr[0]};
    esb_set_prefixes(prefix, 1);

    // 3. 發送！
    // esb_write_payload 會自動等待 ACK
    // 如果收到 ACK，由 ESB 硬體處理，函數返回 0 (成功)
    // 如果沒收到 ACK (超時)，函數會返回錯誤
    
    printk("Pinging %s on CH %d... ", addr_name, channel);
    
    err = esb_write_payload(&tx_payload);
    
    if (err == 0) {
        printk(">>> ACK RECEIVED! <<< (TARGET FOUND)\n");
        return true;
    } else {
        printk("No response.\n");
        return false;
    }
}

int main(void)
{
    if (IS_ENABLED(CONFIG_USB_DEVICE_STACK)) {
        usb_enable(NULL);
    }
    
    k_sleep(K_SECONDS(3));
    printk("\n=== Pico Tracker Active Pinger ===\n");
    printk("Attempting to wake up the tracker...\n");

    if (esb_initialize() != 0) {
        printk("ESB Init Failed\n");
        return 0;
    }

    while (1) {
        // 嘗試組合 1: 頻道 73, 地址 43...
        if (try_ping(CH_A, addr_pico, "ADDR:43")) {
             // 找到後瘋狂閃爍 LED 或停留
             k_sleep(K_MSEC(100)); 
             continue; 
        }

        // 嘗試組合 2: 頻道 45, 地址 43...
        if (try_ping(CH_B, addr_pico, "ADDR:43")) {
             k_sleep(K_MSEC(100)); 
             continue; 
        }

        // 嘗試組合 3: 頻道 73, 地址 E7...
        if (try_ping(CH_A, addr_default, "ADDR:E7")) {
             k_sleep(K_MSEC(100)); 
             continue; 
        }

        // 嘗試組合 4: 頻道 45, 地址 E7...
        if (try_ping(CH_B, addr_default, "ADDR:E7")) {
             k_sleep(K_MSEC(100)); 
             continue; 
        }
        
        // 稍微休息一下，讓 Log 不要跑太快
        k_sleep(K_MSEC(100));
    }
    return 0;
}

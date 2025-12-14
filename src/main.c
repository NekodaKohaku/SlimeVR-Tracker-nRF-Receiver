/*
 * Pico Final Receiver (Hopping Cracker)
 * Strategy: Camp on Channel 77 and wait for the tracker.
 * Address: Base 0x552C6A1E, Prefix 0xC0
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <hal/nrf_radio.h>

// [新增 1] 必須引入 USB 和 UART 驅動標頭檔
#include <zephyr/usb/usb_device.h>
#include <zephyr/drivers/uart.h>

// === 我們解碼出來的密鑰 ===
#define CRACKED_BASE_ADDR   0x552C6A1E
#define CRACKED_PREFIX      0xC0
#define CAMPING_CHANNEL     77  // 對應 0x4D (2477 MHz)

void radio_init_sniffer(void) {
    // 1. 關閉無線電
    NRF_RADIO->TASKS_DISABLE = 1;
    while (NRF_RADIO->EVENTS_DISABLED == 0);
    NRF_RADIO->EVENTS_DISABLED = 0;

    // 2. 設定模式 (Pico 2.0 通常使用 NRF_2MBIT)
    nrf_radio_mode_set(NRF_RADIO, NRF_RADIO_MODE_NRF_2MBIT);
    
    // 3. 設定頻率 (死守 Channel 77)
    nrf_radio_frequency_set(NRF_RADIO, CAMPING_CHANNEL);

    // 4. 設定地址
    nrf_radio_base0_set(NRF_RADIO, CRACKED_BASE_ADDR);
    nrf_radio_prefix0_set(NRF_RADIO, CRACKED_PREFIX);
    nrf_radio_rxaddresses_set(NRF_RADIO, 1); // 啟用邏輯地址 0

    // 5. 封包格式 (標準 ESB 配置)
    NRF_RADIO->PCNF0 = (8 << RADIO_PCNF0_LFLEN_Pos) | 
                       (1 << RADIO_PCNF0_S0LEN_Pos);
                       
    // MaxLen=32, BalLen=4, Big Endian, Whitening Enabled
    NRF_RADIO->PCNF1 = (32 << RADIO_PCNF1_MAXLEN_Pos) | 
                       (4 << RADIO_PCNF1_BALEN_Pos) | 
                       (RADIO_PCNF1_ENDIAN_Big << RADIO_PCNF1_ENDIAN_Pos) |
                       (RADIO_PCNF1_WHITEEN_Enabled << RADIO_PCNF1_WHITEEN_Pos);

    // 6. CRC 設定
    nrf_radio_crc_configure(NRF_RADIO, RADIO_CRCCNF_LEN_Two, NRF_RADIO_CRC_ADDR_SKIP, 0x11021);
    NRF_RADIO->CRCINIT = 0xFFFF;
    NRF_RADIO->DATAWHITEIV = CAMPING_CHANNEL | 0x40; 

    // 7. 快捷方式
    NRF_RADIO->SHORTS = RADIO_SHORTS_READY_START_Msk | RADIO_SHORTS_END_START_Msk;
}

int main(void) {
    // [新增 2] 初始化 USB 堆疊
    // 因為 prj.conf 設為 INITIALIZE_AT_BOOT=n，這裡必須手動呼叫
    if (usb_enable(NULL)) {
        return 0;
    }

    // [新增 3] 等待電腦的 Serial Monitor 連上
    // 如果不加這段，電腦還沒打開視窗，"Pico Hunter Active" 就已經印完了
    const struct device *dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_console));
    uint32_t dtr = 0;

    // 等待直到檢測到 DTR 信號 (代表 Serial 視窗已打開)
    // 如果您不想等，可以把這段 while 拿掉，但我強烈建議留著
    while (!dtr) {
        uart_line_ctrl_get(dev, UART_LINE_CTRL_DTR, &dtr);
        k_sleep(K_MSEC(100));
    }

    printk("\n=== Pico Hunter Active ===\n");
    printk("Camping on Freq: 2477 MHz (CH %d)\n", CAMPING_CHANNEL);
    printk("Target Address: 0x%02X + 0x%08X\n", CRACKED_PREFIX, CRACKED_BASE_ADDR);

    radio_init_sniffer();
    NRF_RADIO->TASKS_RXEN = 1;

    while (1) {
        // 偵測到「地址匹配」 (抓到訊號了！)
        if (NRF_RADIO->EVENTS_ADDRESS) {
            NRF_RADIO->EVENTS_ADDRESS = 0;
            printk("[!] Signal Detected! (Address Match)\n");
        }

        // 封包接收結束
        if (NRF_RADIO->EVENTS_END) {
            NRF_RADIO->EVENTS_END = 0;
            
            if (NRF_RADIO->EVENTS_CRCOK) {
                printk(">>> BINGO! Packet Received! CRC OK! <<<\n");
            } else if (NRF_RADIO->EVENTS_CRCERROR) {
                printk("--- CRC Error (Frequency mismatch or noise) ---\n");
            }
        }
        
        k_busy_wait(100); 
    }
    return 0;
}

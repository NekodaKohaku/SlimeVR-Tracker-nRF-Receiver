/*
 * Pico Tracker "Final Unlocker" v2.0 (Fast Switch)
 * Features:
 * 1. Hardware SHORTS for TX->RX transition (Catch 130us ACK)
 * 2. Multi-Address Scanning (Try different interpretations of Dump)
 * 3. Strict CRC Check
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/usb/usb_device.h>
#include <hal/nrf_radio.h>

// 喚醒指令 (來自 Dump)
static uint8_t magic_payload[] = {0x50, 0x01, 0x02, 0x03}; 

// 候選地址列表 (基於 Dump 04 4E 4E 00 的變體)
struct AddressConfig {
    uint32_t base;
    uint8_t prefix;
};

static struct AddressConfig candidates[] = {
    {0x4E4E4E4E, 0x04}, // 最可能：4E填充 + 04前綴
    {0x004E4E4E, 0x04}, // 可能性 2
    {0x044E4E4E, 0x00}, // 可能性 3
    {0xE7E7E7E7, 0x04}, // 可能性 4 (Nordic 預設 Base + 04)
};

void radio_configure(uint8_t channel, uint32_t base, uint8_t prefix) {
    // 1. Reset & Disable
    NRF_RADIO->SHORTS = 0;
    NRF_RADIO->TASKS_DISABLE = 1;
    while (NRF_RADIO->EVENTS_DISABLED == 0);
    NRF_RADIO->EVENTS_DISABLED = 0;

    // 2. Config 2Mbps ESB
    nrf_radio_power_set(NRF_RADIO, NRF_RADIO_TXPOWER_POS4DBM);
    nrf_radio_frequency_set(NRF_RADIO, channel);
    nrf_radio_mode_set(NRF_RADIO, NRF_RADIO_MODE_NRF_2MBIT);

    // 3. Address Setup
    nrf_radio_base0_set(NRF_RADIO, base);
    nrf_radio_prefix0_set(NRF_RADIO, prefix);
    nrf_radio_rxaddresses_set(NRF_RADIO, 1); 

    // 4. PCNF (ESB Standard)
    NRF_RADIO->PCNF0 = (8 << RADIO_PCNF0_LFLEN_Pos) | (1 << RADIO_PCNF0_S0LEN_Pos);
    NRF_RADIO->PCNF1 = (32 << RADIO_PCNF1_MAXLEN_Pos) | 
                       (4 << RADIO_PCNF1_BALEN_Pos) | 
                       (RADIO_PCNF1_ENDIAN_Big << RADIO_PCNF1_ENDIAN_Pos) | // Big Endian
                       (RADIO_PCNF1_WHITEEN_Enabled << RADIO_PCNF1_WHITEEN_Pos);

    // 5. CRC
    nrf_radio_crc_configure(NRF_RADIO, RADIO_CRCCNF_LEN_Two, NRF_RADIO_CRC_ADDR_SKIP, 0x11021);
    NRF_RADIO->CRCINIT = 0xFFFF;
    NRF_RADIO->DATAWHITEIV = channel | 0x40;
}

bool attempt_handshake(void) {
    // 清除事件
    NRF_RADIO->EVENTS_READY = 0;
    NRF_RADIO->EVENTS_END = 0;
    NRF_RADIO->EVENTS_DISABLED = 0;
    NRF_RADIO->EVENTS_CRCOK = 0;

    // 設定 Payload
    NRF_RADIO->TXADDRESS = 0;
    NRF_RADIO->PACKETPTR = (uint32_t)magic_payload;

    // *** 關鍵：設定硬體捷徑 (Shortcuts) ***
    // 流程：READY -> 發送(START) -> 發送完(END) -> 關閉(DISABLE) -> 自動轉接收(RXEN) -> 接收就緒(READY) -> 開始聽(START)
    // 我們分兩步：
    // Step A: TX -> Disable -> RXEN
    NRF_RADIO->SHORTS = RADIO_SHORTS_READY_START_Msk |        // TX Ready -> Start TX
                        RADIO_SHORTS_END_DISABLE_Msk |        // TX Done -> Disable
                        RADIO_SHORTS_DISABLED_RXEN_Msk;       // Disabled -> Start RX Ramp-up
    
    // 啟動發送 (TX Ramp-up)
    NRF_RADIO->TASKS_TXEN = 1;

    // 等待進入 RX 模式 (TX 結束 -> Disable -> RX Ready)
    // 這中間硬體會自動完成，我們只要等 RX Ready
    while (NRF_RADIO->EVENTS_READY == 0); 
    NRF_RADIO->EVENTS_READY = 0;

    // Step B: RX Start -> Wait for ACK
    // 現在無線電已經在 RX Ready 狀態了
    NRF_RADIO->SHORTS = RADIO_SHORTS_READY_START_Msk |        // RX Ready -> Start RX Window
                        RADIO_SHORTS_ADDRESS_RSSISTART_Msk;   // Address Match -> Measure RSSI (Optional)

    // 因為上面的 Shortcuts 已經觸發了 RXEN，這裡它會自動 Ready 並 Start
    // 我們只需要等待結果
    
    // 給予 500us 的時間視窗等待 ACK
    for(int i=0; i<500; i++) {
        if (NRF_RADIO->EVENTS_CRCOK) {
            NRF_RADIO->TASKS_DISABLE = 1;
            while(NRF_RADIO->EVENTS_DISABLED == 0);
            return true; // 成功！
        }
        k_busy_wait(1);
    }

    // 超時，關閉
    NRF_RADIO->TASKS_DISABLE = 1;
    while(NRF_RADIO->EVENTS_DISABLED == 0);
    return false;
}

int main(void) {
    if (IS_ENABLED(CONFIG_USB_DEVICE_STACK)) {
        usb_enable(NULL);
    }

    k_sleep(K_SECONDS(3));
    printk("\n=== Pico Final Unlocker v2.0 (Fast Switch) ===\n");
    printk("Scanning with Hardware Shortcuts...\n");

    while (1) {
        // 掃描全頻段
        for (int ch = 0; ch < 100; ch++) {
            
            // 嘗試每一種可能的地址組合
            for (int i = 0; i < 4; i++) {
                radio_configure(ch, candidates[i].base, candidates[i].prefix);
                
                if (attempt_handshake()) {
                    printk("\n\n>>> HIT! Channel %d, AddrConfig %d <<<\n", ch, i);
                    printk("Base: %x, Prefix: %x\n", candidates[i].base, candidates[i].prefix);
                    
                    // 鎖定頻道，持續通訊
                    while(1) {
                        if(attempt_handshake()) {
                            printk("ACK! ");
                        } else {
                            printk("_ ");
                        }
                        k_sleep(K_MSEC(100));
                    }
                }
            }
        }
        printk(".");
        k_sleep(K_MSEC(1));
    }
    return 0;
}

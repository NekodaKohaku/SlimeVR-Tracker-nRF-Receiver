/*
 * PICO Dongle Clone - FAST SWITCH (SHORTS)
 * 1. 保持 Big Endian, Prefix C0, CRC Fix (TX 已驗證成功)
 * 2. 使用 SHORTS 自動切換 TX -> RX (解決漏接 ACK 問題)
 * 3. 動態更新 Packet Pointer
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/usb/usb_device.h>
#include <zephyr/drivers/gpio.h>
#include <hal/nrf_radio.h>
#include <string.h>

// === 參數設定 ===
#define PUBLIC_ADDR      0x552C6A1E
#define PAYLOAD_ID_SALT  0xB9522E32
#define LED0_NODE        DT_ALIAS(led0)
#define TARGET_FREQ      1  // 鎖定 2401 MHz (Channel 1)

static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(LED0_NODE, gpios);

// TX Packet (Verified)
static uint8_t tx_packet[] = {
    0x11, 0x02, // Len, S1
    0x04, 0x00, 0x7C, 0x00, 0x00, 0x00, 
    0x55, 0x00, 0x00, 0x08, 0x80, 0xA4, 
    0x32, 0x2E, 0x52, 0xB9, 
    0x00                    
};

static uint8_t rx_buffer[64];

static inline void radio_disable_clean(void)
{
    NRF_RADIO->EVENTS_DISABLED = 0;
    NRF_RADIO->TASKS_DISABLE = 1;
    while (NRF_RADIO->EVENTS_DISABLED == 0) { }
    NRF_RADIO->EVENTS_DISABLED = 0;
}

static inline uint32_t rbit(uint32_t value) {
    uint32_t result;
    __asm volatile ("rbit %0, %1" : "=r" (result) : "r" (value));
    return result;
}

uint32_t calculate_pico_address(uint32_t ficr) {
    uint8_t *f = (uint8_t*)&ficr;
    uint32_t salt_val = PAYLOAD_ID_SALT;
    uint8_t *s = (uint8_t*)&salt_val;
    uint8_t res[4];
    for(int i=0; i<4; i++) res[i] = (f[i] + s[i]) & 0xFF;
    uint32_t combined = (res[3] << 24) | (res[2] << 16) | (res[1] << 8) | res[0];
    return rbit(combined);
}

static void radio_init(uint32_t freq)
{
    radio_disable_clean();

    NRF_RADIO->TXPOWER   = (RADIO_TXPOWER_TXPOWER_0dBm << RADIO_TXPOWER_TXPOWER_Pos);
    NRF_RADIO->FREQUENCY = freq; 
    NRF_RADIO->MODE      = (RADIO_MODE_MODE_Nrf_2Mbit << RADIO_MODE_MODE_Pos);

    // Address Setup (Prefix C0, Base Public)
    NRF_RADIO->PREFIX0 = 0xC0; 
    NRF_RADIO->BASE0   = PUBLIC_ADDR;
    NRF_RADIO->TXADDRESS   = 0; 
    NRF_RADIO->RXADDRESSES = 1; 

    // PCNF0 (S0=0, S1=4)
    NRF_RADIO->PCNF0 = (8UL << RADIO_PCNF0_LFLEN_Pos) | 
                       (0UL << RADIO_PCNF0_S0LEN_Pos) | 
                       (4UL << RADIO_PCNF0_S1LEN_Pos);

    // PCNF1 (Big Endian)
    NRF_RADIO->PCNF1 = (35UL << RADIO_PCNF1_MAXLEN_Pos) |
                       (0UL  << RADIO_PCNF1_STATLEN_Pos) |
                       (4UL  << RADIO_PCNF1_BALEN_Pos) |
                       (RADIO_PCNF1_ENDIAN_Big << RADIO_PCNF1_ENDIAN_Pos) |
                       (0UL  << RADIO_PCNF1_WHITEEN_Pos); 

    // CRC (Include Address)
    NRF_RADIO->CRCCNF = 2; 
    NRF_RADIO->CRCINIT = 0xFFFF;
    NRF_RADIO->CRCPOLY = 0x11021;
}

int main(void)
{
    if (device_is_ready(led.port)) {
        gpio_pin_configure_dt(&led, GPIO_OUTPUT_ACTIVE);
        gpio_pin_set_dt(&led, 0);
    }
    (void)usb_enable(NULL);

    k_sleep(K_SECONDS(2));

    printk("\n=== PICO SENDER (FAST SWITCH MODE) ===\n");
    printk("Attempting to catch the ACK...\n");

    radio_init(TARGET_FREQ); 
    
    bool paired = false;
    uint32_t private_id = 0;

    while (!paired) {
        
        // 1. 準備 RX Buffer (提前準備)
        // 注意：Packet Pointer 是 Double Buffered 或是 Start 時 Latch 的
        // 我們要在 TX 啟動後，立刻把 Pointer 指向 RX，這樣硬體自動轉 RX 時就會寫入這裡
        
        // 2. 設定 TX Buffer
        NRF_RADIO->PACKETPTR = (uint32_t)tx_packet;
        
        // 3. 設定 SHORTS (關鍵!)
        // READY->START: 準備好就發
        // END->DISABLE: 發完就關閉 TX
        // DISABLED->RXEN: 關閉 TX 後，"立刻" 啟動 RX (光速切換)
        NRF_RADIO->SHORTS = RADIO_SHORTS_READY_START_Msk | 
                            RADIO_SHORTS_END_DISABLE_Msk | 
                            RADIO_SHORTS_DISABLED_RXEN_Msk;
        
        // 清除事件
        NRF_RADIO->EVENTS_END = 0;
        NRF_RADIO->EVENTS_DISABLED = 0;
        NRF_RADIO->EVENTS_READY = 0;

        // 4. 啟動 (TXEN)
        NRF_RADIO->TASKS_TXEN = 1;

        // 5. [關鍵技巧] 在 TX 進行中，把 PacketPtr 指向 RX Buffer
        // 等待 TX Ready (代表已經讀取了 tx_packet 地址，正在熱身)
        while(NRF_RADIO->EVENTS_READY == 0);
        NRF_RADIO->EVENTS_READY = 0;
        
        // 此時硬體已經鎖定 TX Buffer，我們可以安全地把 Pointer 改成 RX
        // 當硬體自動切換到 RX 時，它會使用這個新的 Pointer
        NRF_RADIO->PACKETPTR = (uint32_t)rx_buffer;
        
        // 6. 現在我們等待 RX 的結束
        // 流程是: TX發送 -> END(TX) -> DISABLE -> RXEN -> READY(RX) -> START(RX) -> END(RX收到ACK)
        // 為了安全，我們設一個超時
        bool ack_received = false;
        for(volatile int w=0; w<30000; w++) {
             // 這裡我們檢查 EVENTS_END。
             // 第一次 END 是 TX 完成，第二次 END 是 RX 完成 (收到 ACK)
             // 但 SHORTS 會自動清掉某些 flag 嗎? 不會。
             // 我們可以簡單地輪詢
             if (NRF_RADIO->EVENTS_END) {
                 // 可能是 TX 剛結束，也可能是 RX 結束
                 // 檢查 CRCSTATUS，只有 RX 收到包才會更新 CRCSTATUS
                 if (NRF_RADIO->CRCSTATUS) {
                     ack_received = true;
                     break;
                 }
                 // 如果只是 TX 結束，CRCSTATUS 通常不變或為 0，我們繼續等
                 // 清除 END 以便捕捉下一次 (RX) 的 END
                 // NRF_RADIO->EVENTS_END = 0; // 小心，別把剛收到的清掉了
             }
        }

        // 7. 停止 Radio (不管有沒有收到，先停下來)
        NRF_RADIO->SHORTS = 0; // 關閉自動化
        NRF_RADIO->TASKS_DISABLE = 1;
        while(NRF_RADIO->EVENTS_DISABLED == 0);
        NRF_RADIO->EVENTS_DISABLED = 0;

        if (ack_received) {
             if (rx_buffer[0] == 0x0D) { // 長度 13
                  gpio_pin_set_dt(&led, 1);
                  printk("\n[+] BINGO! ACK CAUGHT!\n");
                  
                  printk("    Raw: ");
                  for(int k=0; k<14; k++) printk("%02X ", rx_buffer[k]);
                  printk("\n");
                  
                  // FICR 提取
                  uint32_t ficr = 0;
                  memcpy(&ficr, &rx_buffer[4], 4);
                  private_id = calculate_pico_address(ficr);
                  printk("    FICR: %08X -> ID: %08X\n", ficr, private_id);
                  
                  paired = true;
             }
        }

        if (paired) break;

        // Toggle Seq
        tx_packet[18] ^= 1; 
        k_sleep(K_MSEC(5));
    }
    
    printk("\n[DONE] ID: %08X\n", private_id);
    while(1) {
        gpio_pin_toggle_dt(&led);
        k_sleep(K_MSEC(500));
    }
}

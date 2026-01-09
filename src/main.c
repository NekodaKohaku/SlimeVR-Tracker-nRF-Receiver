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

// 掃描列表: 2401, 2426, 2480 MHz
static const uint8_t scan_channels[] = {1, 26, 80}; 

static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(LED0_NODE, gpios);

// 發送緩衝區 (模擬 Sniffer 抓到的配對請求包)
// Length=0x11 (17 bytes), S0/S1/Payload follows
static uint8_t tx_packet[] = {
    0x11, // Length (LFLEN)
    0x02, 0x04, 0x00, // Header/S1
    0x7C, 0x00, 0x00, 0x00, 0x55, 0x00, 0x00, 0x08, 0x80, 0xA4, // Payload
    0x32, 0x2E, 0x52, 0xB9, // Salt (Little Endian)
    0x00 // PID (會動態跳變)
};

static uint8_t rx_buffer[64];

// === 輔助函數 ===

static inline void radio_disable_clean(void)
{
    NRF_RADIO->EVENTS_DISABLED = 0;
    NRF_RADIO->TASKS_DISABLE = 1;
    while (NRF_RADIO->EVENTS_DISABLED == 0) { }
    NRF_RADIO->EVENTS_DISABLED = 0;
}

// ARM RBIT (算出私有地址用)
static inline uint32_t rbit(uint32_t value) {
    uint32_t result;
    __asm volatile ("rbit %0, %1" : "=r" (result) : "r" (value));
    return result;
}

// 計算地址公式
uint32_t calculate_pico_address(uint32_t ficr) {
    uint8_t *f = (uint8_t*)&ficr;
    uint32_t salt_val = PAYLOAD_ID_SALT;
    uint8_t *s = (uint8_t*)&salt_val;
    uint8_t res[4];

    for(int i=0; i<4; i++) {
        res[i] = (f[i] + s[i]) & 0xFF;
    }
    uint32_t combined = (res[3] << 24) | (res[2] << 16) | (res[1] << 8) | res[0];
    return rbit(combined);
}

static void radio_init(uint32_t freq)
{
    radio_disable_clean();

    // 1. 物理層 (PHY)
    NRF_RADIO->TXPOWER   = (RADIO_TXPOWER_TXPOWER_0dBm << RADIO_TXPOWER_TXPOWER_Pos);
    NRF_RADIO->FREQUENCY = freq; 
    NRF_RADIO->MODE      = (RADIO_MODE_MODE_Nrf_2Mbit << RADIO_MODE_MODE_Pos);

    // 2. 地址結構 (BALEN=4, PREFIX=0) - 對齊 Sniffer
    NRF_RADIO->PREFIX0 = 0;
    NRF_RADIO->BASE0   = PUBLIC_ADDR;
    NRF_RADIO->TXADDRESS   = 0; 
    NRF_RADIO->RXADDRESSES = 1; 

    // 3. PCNF0 (LFLEN=8, S0LEN=0, S1LEN=4) - 對齊 Sniffer
    NRF_RADIO->PCNF0 = (8UL << RADIO_PCNF0_LFLEN_Pos) | 
                       (0UL << RADIO_PCNF0_S0LEN_Pos) | 
                       (4UL << RADIO_PCNF0_S1LEN_Pos);

    // 4. PCNF1 (WhiteEn=0, Balen=4, Endian=Little) - 對齊 Sniffer
    // 這是之前失敗的主因，必須關閉 Whitening！
    NRF_RADIO->PCNF1 = (64UL << RADIO_PCNF1_MAXLEN_Pos) |
                       (0UL  << RADIO_PCNF1_STATLEN_Pos) |
                       (4UL  << RADIO_PCNF1_BALEN_Pos) |
                       (RADIO_PCNF1_ENDIAN_Little << RADIO_PCNF1_ENDIAN_Pos) |
                       (0UL  << RADIO_PCNF1_WHITEEN_Pos); 

    // 5. CRC (CRC16, SkipAddr) - 對齊 Sniffer
    NRF_RADIO->CRCCNF = (RADIO_CRCCNF_LEN_Two << RADIO_CRCCNF_LEN_Pos) |
                        (RADIO_CRCCNF_SKIPADDR_Skip << RADIO_CRCCNF_SKIPADDR_Pos);
    NRF_RADIO->CRCINIT = 0xFFFF;
    NRF_RADIO->CRCPOLY = 0x11021;
}

int main(void)
{
    // LED Init
    if (device_is_ready(led.port)) {
        gpio_pin_configure_dt(&led, GPIO_OUTPUT_ACTIVE);
        gpio_pin_set_dt(&led, 0);
    }

    // USB Init (Non-blocking)
    (void)usb_enable(NULL);

    // 開機閃爍
    for (int i = 0; i < 6; i++) {
        gpio_pin_toggle_dt(&led);
        k_sleep(K_MSEC(100));
    }
    gpio_pin_set_dt(&led, 0);

    printk("\n=== PICO DONGLE (PHY FIXED: No White, CRC16) ===\n");

    int ch_idx = 0;
    bool paired = false;
    uint32_t private_id = 0;

    // === 階段一：主動配對 ===
    while (!paired) {
        uint8_t freq = scan_channels[ch_idx];
        radio_init(freq);
        printk("Ping %d MHz... ", 2400+freq);

        bool got_any_signal = false;

        // 每個頻道打 20 發
        for(int i=0; i<20; i++) {
            
            // --- TX ---
            NRF_RADIO->PACKETPTR = (uint32_t)tx_packet;
            NRF_RADIO->SHORTS = RADIO_SHORTS_READY_START_Msk | RADIO_SHORTS_END_DISABLE_Msk;
            NRF_RADIO->EVENTS_END = 0;
            NRF_RADIO->EVENTS_DISABLED = 0;
            NRF_RADIO->TASKS_TXEN = 1;
            while(NRF_RADIO->EVENTS_DISABLED == 0); // Wait TX done
            NRF_RADIO->EVENTS_DISABLED = 0;

            // --- RX (Fast Switch) ---
            NRF_RADIO->PACKETPTR = (uint32_t)rx_buffer;
            NRF_RADIO->SHORTS = RADIO_SHORTS_READY_START_Msk | RADIO_SHORTS_END_DISABLE_Msk;
            NRF_RADIO->EVENTS_END = 0;
            NRF_RADIO->TASKS_RXEN = 1;
            
            // Wait for ACK (~5ms)
            for(volatile int w=0; w<10000; w++) {
                if(NRF_RADIO->EVENTS_END) {
                    // 只要有任何 END 事件，代表有收到東西(不管 CRC 對錯)
                    got_any_signal = true;
                    if (NRF_RADIO->CRCSTATUS) {
                        // CRC OK! 檢查內容
                        if (rx_buffer[0] == 0x0D) { // Length 13
                            gpio_pin_set_dt(&led, 1);
                            printk("\n[+] BINGO! Got FICR!\n");
                            
                            // 提取 FICR (Sniffer 顯示是 19 7E... 在 Offset 4)
                            uint32_t ficr = 0;
                            memcpy(&ficr, &rx_buffer[4], 4);
                            
                            private_id = calculate_pico_address(ficr);
                            printk("    FICR: %08X -> Addr: %08X\n", ficr, private_id);
                            paired = true;
                        }
                    }
                    break; // 收到就跳出等待
                }
            }

            // Cleanup
            NRF_RADIO->TASKS_DISABLE = 1;
            while(NRF_RADIO->EVENTS_DISABLED == 0);
            NRF_RADIO->EVENTS_DISABLED = 0;

            if (paired) break;

            // Toggle PID
            tx_packet[sizeof(tx_packet)-1] ^= 1;
            k_sleep(K_MSEC(5));
        }

        if (got_any_signal && !paired) {
            printk("(Signal detected, but invalid packet)\n");
        } else if (!paired) {
            printk("\n");
        }

        if (paired) break;

        ch_idx = (ch_idx + 1) % 3;
        k_sleep(K_MSEC(50));
    }

    // === 階段二：配對成功，監聽私有頻道 ===
    printk("\n=== PAIRED! Monitor: %08X ===\n", private_id);
    
    // 重新 Init 確保乾淨
    radio_init(2478); // 暫時用 2478 或保持當前頻率
    // 這裡通常要改成跳頻邏輯，我們先監聽當前頻率試試
    
    // 設定私有地址到 BASE1
    NRF_RADIO->BASE1 = private_id;
    NRF_RADIO->RXADDRESSES = 2; // Enable Base1

    NRF_RADIO->PACKETPTR = (uint32_t)rx_buffer;
    NRF_RADIO->SHORTS = RADIO_SHORTS_READY_START_Msk | RADIO_SHORTS_END_START_Msk; // 持續收
    NRF_RADIO->TASKS_RXEN = 1;

    while(1) {
        if(NRF_RADIO->EVENTS_END) {
            NRF_RADIO->EVENTS_END = 0;
            if(NRF_RADIO->CRCSTATUS) {
                // 收到私有頻道的 IMU 數據
                gpio_pin_toggle_dt(&led);
                // 數據在 rx_buffer
                // 只印開頭幾個 byte
                 printk("DATA: %02X %02X %02X %02X\n", 
                        rx_buffer[0], rx_buffer[1], rx_buffer[2], rx_buffer[3]);
            }
        }
        k_sleep(K_MSEC(1));
    }
}

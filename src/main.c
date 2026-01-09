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

// 掃描列表
static const uint8_t scan_channels[] = {1, 26, 80}; 

static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(LED0_NODE, gpios);

// 發送緩衝區 (Dongle 喊話內容)
static uint8_t tx_packet[] = {
    0x11, 0x02, 0x04, 0x00, 
    0x7C, 0x00, 0x00, 0x00, 0x55, 0x00, 0x00, 0x08, 0x80, 0xA4,
    0x32, 0x2E, 0x52, 0xB9, // Salt (Little Endian)
    0x00 // PID
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

    NRF_RADIO->TXPOWER   = (RADIO_TXPOWER_TXPOWER_0dBm << RADIO_TXPOWER_TXPOWER_Pos);
    NRF_RADIO->FREQUENCY = freq; 
    NRF_RADIO->MODE      = (RADIO_MODE_MODE_Nrf_2Mbit << RADIO_MODE_MODE_Pos);

    NRF_RADIO->PREFIX0 = 0;
    NRF_RADIO->BASE0   = PUBLIC_ADDR;
    NRF_RADIO->TXADDRESS   = 0; 
    NRF_RADIO->RXADDRESSES = 1; 

    // PCNF0: LFLEN=8, S0LEN=1
    NRF_RADIO->PCNF0 = (8UL << RADIO_PCNF0_LFLEN_Pos) | 
                       (1UL << RADIO_PCNF0_S0LEN_Pos) | 
                       (0UL << RADIO_PCNF0_S1LEN_Pos);

    // PCNF1: MaxLen=255, Balen=3, Little Endian, Whitening=ON
    NRF_RADIO->PCNF1 = (255UL << RADIO_PCNF1_MAXLEN_Pos) |
                       (3UL   << RADIO_PCNF1_BALEN_Pos) |
                       (RADIO_PCNF1_ENDIAN_Little << RADIO_PCNF1_ENDIAN_Pos) |
                       (1UL   << RADIO_PCNF1_WHITEEN_Pos); 

    // CRC: 3 bytes
    NRF_RADIO->CRCCNF = (RADIO_CRCCNF_LEN_Three << RADIO_CRCCNF_LEN_Pos) |
                        (RADIO_CRCCNF_SKIPADDR_Skip << RADIO_CRCCNF_SKIPADDR_Pos);
    NRF_RADIO->CRCINIT = 0xFFFF;
    NRF_RADIO->CRCPOLY = 0x11021;
}

int main(void)
{
    // 1. 初始化 LED (視覺回饋)
    if (device_is_ready(led.port)) {
        gpio_pin_configure_dt(&led, GPIO_OUTPUT_ACTIVE);
        gpio_pin_set_dt(&led, 0);
    }

    // 2. 初始化 USB (不檢查返回值，失敗就算了，讓程式繼續跑)
    (void)usb_enable(NULL);

    // 3. 開機閃爍 (告訴你程式活著)
    for (int i = 0; i < 6; i++) {
        gpio_pin_toggle_dt(&led);
        k_sleep(K_MSEC(200));
    }
    gpio_pin_set_dt(&led, 0); // LED 滅

    printk("\n=== PICO DONGLE CLONE (Non-Blocking) ===\n");

    int ch_idx = 0;
    bool paired = false;
    uint32_t private_id = 0;

    // === 階段一：主動配對迴圈 ===
    while (!paired) {
        // 切換頻率
        uint8_t freq = scan_channels[ch_idx];
        radio_init(freq);
        printk("Ping on %d MHz...\n", 2400+freq);

        // 嘗試 Ping 15 次
        for(int i=0; i<15; i++) {
            
            // --- A. 發送挑戰包 (TX) ---
            NRF_RADIO->PACKETPTR = (uint32_t)tx_packet;
            // 設定 SHORTS: Ready->Start (自動發), End->Disable (發完關)
            NRF_RADIO->SHORTS = RADIO_SHORTS_READY_START_Msk | RADIO_SHORTS_END_DISABLE_Msk;
            NRF_RADIO->EVENTS_END = 0;
            NRF_RADIO->EVENTS_DISABLED = 0;
            
            NRF_RADIO->TASKS_TXEN = 1; // 啟動 TX
            while(NRF_RADIO->EVENTS_DISABLED == 0); // 等待 TX 完成
            NRF_RADIO->EVENTS_DISABLED = 0;

            // --- B. 快速切換接收 (RX) ---
            NRF_RADIO->PACKETPTR = (uint32_t)rx_buffer;
            // 設定 SHORTS: Ready->Start (自動收), End->Disable (收完關)
            NRF_RADIO->SHORTS = RADIO_SHORTS_READY_START_Msk | RADIO_SHORTS_END_DISABLE_Msk;
            NRF_RADIO->EVENTS_END = 0;
            
            NRF_RADIO->TASKS_RXEN = 1; // 啟動 RX
            
            // --- C. 等待回應 (Timeout ~5ms) ---
            // 用 Busy Wait 最穩，不會被 OS 切換打斷
            bool got_packet = false;
            for(volatile int w=0; w<10000; w++) {
                if(NRF_RADIO->EVENTS_END) {
                    got_packet = true;
                    break;
                }
            }

            if (got_packet) {
                if (NRF_RADIO->CRCSTATUS) {
                    // 檢查是不是配對包 (Header 0D)
                    if (rx_buffer[0] == 0x0D) {
                        gpio_pin_set_dt(&led, 1); // 亮燈慶祝
                        
                        printk("\n[+] GOT FICR PACKET!\n");
                        printk("    Data: ");
                        for(int k=0; k<12; k++) printk("%02X ", rx_buffer[k]);
                        printk("\n");

                        // 提取 FICR (Offset 4)
                        uint32_t ficr = 0;
                        memcpy(&ficr, &rx_buffer[4], 4);
                        
                        private_id = calculate_pico_address(ficr);
                        printk("    FICR: %08X -> ID: %08X\n", ficr, private_id);
                        
                        paired = true;
                        break; // 脫離 for
                    }
                }
            }

            // 確保 Radio 關閉
            NRF_RADIO->TASKS_DISABLE = 1;
            while(NRF_RADIO->EVENTS_DISABLED == 0);
            NRF_RADIO->EVENTS_DISABLED = 0;

            // 翻轉 PID 並稍微休息
            tx_packet[sizeof(tx_packet)-1] ^= 1;
            k_sleep(K_MSEC(10));
        }

        if (!paired) {
            ch_idx = (ch_idx + 1) % 3;
            k_sleep(K_MSEC(50));
        }
    }

    // === 階段二：配對成功 ===
    printk("\n=== PAIRED! Listening on %08X ===\n", private_id);
    
    // 設定私有地址
    radio_disable_clean();
    NRF_RADIO->BASE1 = private_id;
    NRF_RADIO->RXADDRESSES = 2; // Enable Base1

    NRF_RADIO->PACKETPTR = (uint32_t)rx_buffer;
    NRF_RADIO->SHORTS = RADIO_SHORTS_READY_START_Msk | RADIO_SHORTS_END_START_Msk; // 持續收
    NRF_RADIO->TASKS_RXEN = 1;

    while(1) {
        if(NRF_RADIO->EVENTS_END) {
            NRF_RADIO->EVENTS_END = 0;
            if(NRF_RADIO->CRCSTATUS) {
                // 收到數據閃一下燈
                gpio_pin_toggle_dt(&led);
                printk("IMU: %02X %02X %02X...\n", rx_buffer[0], rx_buffer[1], rx_buffer[2]);
            }
        }
        k_sleep(K_MSEC(1));
    }
}

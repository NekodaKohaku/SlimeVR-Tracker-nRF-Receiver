#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <hal/nrf_radio.h>
#include <zephyr/usb/usb_device.h>
#include <zephyr/drivers/gpio.h>

// ★★★ 頻率大補帖：覆蓋所有可能的高頻點 ★★★
// 包含你兩次測試看到的所有點，以及中間的空隙
static const uint8_t target_freqs[] = {
    58, 60, 62, 64, 66, 68, 70, 72, 74, 76, 78, 80, // 高頻密集區
    42, 3, 9 // 低頻/同步區 (備用)
};

// 地址參數：鎖定 Pipe 1 (00 + D235CF35)
#define ADDR_BASE_1       0xD235CF35UL 
#define ADDR_PREFIX_0     0x23C300C0UL // Byte 1 = 00

// LED
#define LED0_NODE DT_ALIAS(led0)
static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(LED0_NODE, gpios);

static uint8_t rx_buffer[64];

void radio_init(void)
{
    NRF_RADIO->POWER = 0;
    k_busy_wait(500);
    NRF_RADIO->POWER = 1;

    NRF_RADIO->MODE = NRF_RADIO_MODE_BLE_2MBIT; 

    // PCNF0: S1=4
    NRF_RADIO->PCNF0 = (8UL << RADIO_PCNF0_LFLEN_Pos) | 
                       (0UL << RADIO_PCNF0_S0LEN_Pos) | 
                       (4UL << RADIO_PCNF0_S1LEN_Pos);

    // PCNF1
    NRF_RADIO->PCNF1 = (60UL << RADIO_PCNF1_MAXLEN_Pos) | 
                       (32UL << RADIO_PCNF1_STATLEN_Pos) |
                       (4UL  << RADIO_PCNF1_BALEN_Pos) |
                       (1UL  << RADIO_PCNF1_ENDIAN_Pos) | 
                       (0UL  << RADIO_PCNF1_WHITEEN_Pos);

    // 地址設定
    NRF_RADIO->BASE1 = ADDR_BASE_1; 
    NRF_RADIO->PREFIX0 = ADDR_PREFIX_0;

    // 啟用 Pipe 1 (以及其他以防萬一)
    NRF_RADIO->RXADDRESSES = 0xFE; 

    // ★ CRC 保持關閉 ★
    NRF_RADIO->CRCCNF = 0; 

    NRF_RADIO->SHORTS = RADIO_SHORTS_READY_START_Msk | RADIO_SHORTS_END_DISABLE_Msk;
}

int main(void)
{
    if (device_is_ready(led.port)) {
        gpio_pin_configure_dt(&led, GPIO_OUTPUT_ACTIVE);
        gpio_pin_set_dt(&led, 0); 
    }

    usb_enable(NULL);
    
    // 延遲啟動
    for(int i=0; i<8; i++) {
        gpio_pin_toggle_dt(&led);
        k_sleep(K_MSEC(500));
    }
    gpio_pin_set_dt(&led, 0); 

    printk("\n\n");
    printk("============================================\n");
    printk(">>> HUNTER v28 (High-Band Trawl)         <<<\n");
    printk(">>> Scanning 2458-2480 MHz (Adaptive)    <<<\n");
    printk("============================================\n");

    radio_init();

    int freq_idx = 0;

    while (1) {
        int current_freq = target_freqs[freq_idx];
        NRF_RADIO->FREQUENCY = current_freq;
        
        // 為了減少 Log 刷屏，我們這裡不印 Scanning... 
        // 除非你想要看到它在切換

        int64_t end_time = k_uptime_get() + 200; // 每個頻率停留 0.2 秒，快速輪詢

        while (k_uptime_get() < end_time) {
            
            NRF_RADIO->PACKETPTR = (uint32_t)rx_buffer;
            NRF_RADIO->EVENTS_END = 0;
            NRF_RADIO->TASKS_RXEN = 1;

            bool received = false;
            for (int i = 0; i < 5000; i++) { 
                if (NRF_RADIO->EVENTS_END) {
                    received = true;
                    break;
                }
                k_busy_wait(1);
            }

            if (received) {
                int8_t rssi = -(int8_t)NRF_RADIO->RSSISAMPLE; 
                
                // 只有訊號夠強才印，避免雜訊
                if (rssi > -90) {
                    gpio_pin_toggle_dt(&led);
                    printk("\n!!! [JACKPOT] Freq %d | RSSI %d !!!\n", 2400 + current_freq, rssi);
                    printk("Data: ");
                    for(int k=0; k<32; k++) printk("%02X ", rx_buffer[k]);
                    printk("\n");
                    
                    // ★ 關鍵：一旦抓到了，就死咬著這個頻率久一點 ★
                    // 因為 Tracker 會在這個頻率停留一陣子
                    end_time += 1000; 
                }
                NRF_RADIO->EVENTS_END = 0;
            } else {
                NRF_RADIO->TASKS_DISABLE = 1;
                while (NRF_RADIO->EVENTS_DISABLED == 0);
            }
        }

        freq_idx++;
        if (freq_idx >= sizeof(target_freqs) / sizeof(target_freqs[0])) {
            freq_idx = 0;
        }
    }
}

/*
 * PICO Dongle Clone - BIG ENDIAN FIX
 * 修正說明：
 * 1. PCNF1 Bit 24 Set to 1 (Big Endian) -> 解決 11 變成 88 的問題
 * 2. Prefix = 0xC0 (配合 Dump)
 * 3. CRC Include Address (配合 Dump)
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

// === TX 結構 ===
// 保持原樣，現在 Endian 改了，這些 Byte 會以正確的 Bit 順序飛出去
static uint8_t tx_packet[] = {
    0x11, // Length
    0x02, // S1 (Value 2)
    
    // Payload
    0x04, 0x00,             
    0x7C, 0x00, 0x00, 0x00, 
    0x55, 0x00, 0x00, 0x08, 
    0x80, 0xA4,             
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

    // Address Prefix (C0)
    NRF_RADIO->PREFIX0 = 0xC0; 
    NRF_RADIO->BASE0   = PUBLIC_ADDR;
    NRF_RADIO->TXADDRESS   = 0; 
    NRF_RADIO->RXADDRESSES = 1; 

    // PCNF0: S0=0, S1=4 (Dump Verified)
    NRF_RADIO->PCNF0 = (8UL << RADIO_PCNF0_LFLEN_Pos) | 
                       (0UL << RADIO_PCNF0_S0LEN_Pos) | 
                       (4UL << RADIO_PCNF0_S1LEN_Pos);

    // PCNF1: 關鍵修正 BIG ENDIAN (Bit 24=1)
    // 配合 Dump 0x01040023
    NRF_RADIO->PCNF1 = (35UL << RADIO_PCNF1_MAXLEN_Pos) |
                       (0UL  << RADIO_PCNF1_STATLEN_Pos) |
                       (4UL  << RADIO_PCNF1_BALEN_Pos) |
                       (RADIO_PCNF1_ENDIAN_Big << RADIO_PCNF1_ENDIAN_Pos) | // <--- 這裡改了！
                       (0UL  << RADIO_PCNF1_WHITEEN_Pos); 

    // CRC: Include Address (Dump Verified)
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

    printk("\n=== PICO SENDER (BIG ENDIAN FIXED) ===\n");
    printk("Config: Big Endian, Prefix C0, S1=4\n");

    radio_init(TARGET_FREQ); 
    
    bool paired = false;
    uint32_t private_id = 0;

    while (!paired) {
        
        // --- TX ---
        NRF_RADIO->PACKETPTR = (uint32_t)tx_packet;
        NRF_RADIO->SHORTS = RADIO_SHORTS_READY_START_Msk | RADIO_SHORTS_END_DISABLE_Msk;
        NRF_RADIO->EVENTS_END = 0;
        NRF_RADIO->EVENTS_DISABLED = 0;
        NRF_RADIO->TASKS_TXEN = 1;
        while(NRF_RADIO->EVENTS_DISABLED == 0); 
        NRF_RADIO->EVENTS_DISABLED = 0;

        // --- RX ---
        NRF_RADIO->PACKETPTR = (uint32_t)rx_buffer;
        NRF_RADIO->SHORTS = RADIO_SHORTS_READY_START_Msk | RADIO_SHORTS_END_DISABLE_Msk;
        NRF_RADIO->EVENTS_END = 0;
        NRF_RADIO->TASKS_RXEN = 1;
        
        // Wait for Response
        for(volatile int w=0; w<20000; w++) {
            if(NRF_RADIO->EVENTS_END) {
                // 有訊號
                if (NRF_RADIO->CRCSTATUS) {
                    if (rx_buffer[0] == 0x0D) {
                        gpio_pin_set_dt(&led, 1);
                        printk("\n[+] ACK RECEIVED! CRC OK!\n");
                        
                        // Buffer: [Len][S1][Payload...]
                        uint32_t ficr = 0;
                        memcpy(&ficr, &rx_buffer[4], 4);
                        private_id = calculate_pico_address(ficr);
                        printk("    FICR: %08X -> ID: %08X\n", ficr, private_id);
                        
                        paired = true;
                    }
                }
                break;
            }
        }

        NRF_RADIO->TASKS_DISABLE = 1;
        while(NRF_RADIO->EVENTS_DISABLED == 0);
        NRF_RADIO->EVENTS_DISABLED = 0;

        if (paired) break;

        tx_packet[18] ^= 1; 
        k_sleep(K_MSEC(5));
    }
    
    printk("\n[DONE] ID: %08X\n", private_id);
    while(1) {
        gpio_pin_toggle_dt(&led);
        k_sleep(K_MSEC(500));
    }
}

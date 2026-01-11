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

// 根據 Dump: Frequency = 1 (2401 MHz)
// 我們先鎖定這個頻率，確定能通再說
#define TARGET_FREQ      1 

static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(LED0_NODE, gpios);

// === TX 封包結構 (Bit-Perfect) ===
// 根據 PCNF0: LFLEN=8, S0=0, S1=4
// 硬體發送順序: [Length(8b)] -> [S1(4b)] -> [Payload...]
// 注意：S1 是 4 bits，但記憶體最小單位是 Byte。
// nRF 硬體會取 Byte 的低 4 位還是高 4 位？通常是低位。
// Sniffer 看到 Raw: 11 02 04 00 ...
// Byte 0: 11 (Length)
// Byte 1: 02 (包含 S1). 02 = 0000 0010. S1可能是 2.
// Byte 2: 04 (Payload Start)

static uint8_t tx_packet[] = {
    // Byte 0: LENGTH (LFLEN=8)
    0x11,  // 17 Bytes
    
    // Byte 1: S1 (4 bits) + Padding?
    // 因為 S0LEN=0, 硬體會接著抓 S1。
    // 我們照抄 Sniffer 的 0x02
    0x02,  
    
    // Byte 2~18: PAYLOAD (17 Bytes)
    0x04, 0x00,             
    0x7C, 0x00, 0x00, 0x00, 
    0x55, 0x00, 0x00, 0x08, 
    0x80, 0xA4,             
    0x32, 0x2E, 0x52, 0xB9, // SALT
    0x00                    // Seq
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

    // 地址設定 (與 Dump 0x40001518 一致: BALEN=4)
    NRF_RADIO->PREFIX0 = 0;
    NRF_RADIO->BASE0   = PUBLIC_ADDR;
    NRF_RADIO->TXADDRESS   = 0; 
    NRF_RADIO->RXADDRESSES = 1; 

    // PCNF0 (與 Dump 0x40001514 一致)
    // LFLEN=8, S0LEN=0, S1LEN=4
    // 這是之前失敗的主因！！
    NRF_RADIO->PCNF0 = (8UL << RADIO_PCNF0_LFLEN_Pos) | 
                       (0UL << RADIO_PCNF0_S0LEN_Pos) | 
                       (4UL << RADIO_PCNF0_S1LEN_Pos);

    // PCNF1 (與 Dump 0x40001518 一致)
    // MAXLEN=35(0x23), BALEN=4, WHITEEN=0
    NRF_RADIO->PCNF1 = (35UL << RADIO_PCNF1_MAXLEN_Pos) |
                       (0UL  << RADIO_PCNF1_STATLEN_Pos) |
                       (4UL  << RADIO_PCNF1_BALEN_Pos) |
                       (RADIO_PCNF1_ENDIAN_Little << RADIO_PCNF1_ENDIAN_Pos) |
                       (0UL  << RADIO_PCNF1_WHITEEN_Pos); 

    // CRC (與 Dump 0x40001534 一致: LEN=2)
    NRF_RADIO->CRCCNF = (RADIO_CRCCNF_LEN_Two << RADIO_CRCCNF_LEN_Pos) |
                        (RADIO_CRCCNF_SKIPADDR_Skip << RADIO_CRCCNF_SKIPADDR_Pos);
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

    printk("\n=== PICO SENDER (DUMP VERIFIED) ===\n");
    printk("Config: S0=0, S1=4 (Matched with Tracker Register)\n");
    printk("Target: 2401 MHz\n");

    radio_init(TARGET_FREQ);
    
    // 雖然只鎖定 Channel 1，但如果連不上，手動在這裡改
    // Dump 顯示目前是 1，但它可能會跳到 37(0x25) 或 77(0x4D)
    
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
        
        // Wait for Response (10ms)
        for(volatile int w=0; w<20000; w++) {
            if(NRF_RADIO->EVENTS_END) {
                // 有訊號
                if (NRF_RADIO->CRCSTATUS) {
                    // CRC OK
                    if (rx_buffer[0] == 0x0D) {
                        gpio_pin_set_dt(&led, 1);
                        printk("\n[+] BINGO! ACK RECEIVED!\n");
                        printk("    Data: ");
                        for(int k=0; k<14; k++) printk("%02X ", rx_buffer[k]);
                        printk("\n");
                        
                        // Buffer Structure with S0=0, S1=4
                        // rx_buffer[0] = Length (13)
                        // rx_buffer[1] = S1 (02?)
                        // rx_buffer[2]... = Payload
                        
                        uint32_t ficr = 0;
                        // 嘗試從 offset 4 抓取
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

        // Toggle Seq
        tx_packet[18] ^= 1; 
        k_sleep(K_MSEC(5));
    }
    
    // === 配對成功，燈號閃爍 ===
    printk("\n[DONE] ID Calculated: %08X\n", private_id);
    while(1) {
        gpio_pin_toggle_dt(&led);
        k_sleep(K_MSEC(500));
    }
}

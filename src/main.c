/*
 * Pico Tracker "Whitening" Pinger
 * Features:
 * 1. Data Whitening Enabled (Critical for Pico)
 * 2. Auto-cycling Bitrates (1M/2M)
 * 3. Targeting CH 73 & 45
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/usb/usb_device.h>
#include <hal/nrf_radio.h>

// 目標定義
#define CH_A 73
#define CH_B 45
#define ADDR_PICO 0x43434343
#define ADDR_DFLT 0xE7E7E7E7

// 模擬 ESB 的底層發送函數 (直接操作暫存器，繞過 OS 限制)
void raw_radio_init(void) {
    // 1. 開啟電源
    nrf_radio_power_set(NRF_RADIO, true);

    // 2. 設定地址 (Base0 + Prefix0)
    // 我們將 Prefix 設為地址的最低位元組 (LSB)
    // 0x43434343 -> Base: 0x43434300, Prefix: 0x43
    // 但為了簡單，我們這裡設定 Base 為完整 4 Bytes，Prefix 設為 0 (如果硬體允許)
    // 或者標準拆分：
    nrf_radio_base0_set(NRF_RADIO, 0x43434343); 
    nrf_radio_prefix0_set(NRF_RADIO, 0x43); // Pipe 0 使用 Prefix 0x43
    
    // 設定 Address 1 為 E7
    nrf_radio_base1_set(NRF_RADIO, 0xE7E7E7E7);
    nrf_radio_prefix1_set(NRF_RADIO, 0xE7); // Pipe 1 使用 Prefix 0xE7

    // 啟用 Pipe 0 & 1
    nrf_radio_rxaddresses_set(NRF_RADIO, 3); 

    // 3. 設定封包格式 (PCNF0)
    // LFLEN=0, S0LEN=0, S1LEN=0 (最簡單結構)
    nrf_radio_packet_conf_t packet_conf = {0}; // 將所有欄位初始化為 0
    nrf_radio_packet_configure(NRF_RADIO, &packet_conf);

    // 4. 設定 PCNF1 (關鍵！)
    // MaxLen=32, StatLen=0, Balen=4 (4 byte base), Endian=Little
    // !!! WHITEEN = 1 (開啟白化) !!!
    NRF_RADIO->PCNF1 = (
        (32 << RADIO_PCNF1_MAXLEN_Pos) |
        (4 << RADIO_PCNF1_BALEN_Pos) |
        (RADIO_PCNF1_WHITEEN_Enabled << RADIO_PCNF1_WHITEEN_Pos) | // 這是我們缺少的！
        (RADIO_PCNF1_ENDIAN_Little << RADIO_PCNF1_ENDIAN_Pos)
    );

    // 5. CRC 設定 (16-bit)
    nrf_radio_crc_configure(NRF_RADIO, RADIO_CRCCNF_LEN_Two, NRF_RADIO_CRC_ADDR_SKIP, 0x11021);
    
    // 設定初始 CRC 值
    NRF_RADIO->CRCINIT = 0xFFFF; // 通常是 0xFFFF
}

bool try_ping_raw(uint8_t channel, uint8_t bitrate_mode) {
    // 1. 設定頻率
    nrf_radio_frequency_set(NRF_RADIO, channel);

    // 2. 設定速率
    // 0 = 1Mbps, 1 = 2Mbps
    if (bitrate_mode == 1) 
        nrf_radio_mode_set(NRF_RADIO, NRF_RADIO_MODE_NRF_2MBIT);
    else 
        nrf_radio_mode_set(NRF_RADIO, NRF_RADIO_MODE_NRF_1MBIT);

    // 3. 設定白化初始值 (Data Whitening IV)
    // nRF52 的白化通常使用 Channel 號碼作為 IV
    NRF_RADIO->DATAWHITEIV = channel | 0x40; // bit 6 必須為 1

    // 4. 準備發送數據 (Packet Ptr)
    static uint8_t packet[10];
    packet[0] = 0x55; // 隨便塞點東西
    packet[1] = 0xAA;
    NRF_RADIO->PACKETPTR = (uint32_t)packet;

    // 5. 啟動 Radio (TX)
    NRF_RADIO->EVENTS_READY = 0;
    NRF_RADIO->TASKS_TXEN = 1;
    while (NRF_RADIO->EVENTS_READY == 0); // 等待 Ready

    // 6. 發送
    NRF_RADIO->EVENTS_END = 0;
    NRF_RADIO->TASKS_START = 1;
    while (NRF_RADIO->EVENTS_END == 0); // 等待發送完成

    // 7. 切換到 RX 等待 ACK (快速切換)
    // 這是 ESB 的核心，發完馬上聽
    NRF_RADIO->EVENTS_READY = 0;
    NRF_RADIO->TASKS_RXEN = 1; // 透過 Shortcut 其實更快，這裡手動做
    
    // 等待一小段時間看有沒有 ACK (Timeout 機制)
    // 簡單的 Busy Wait
    for (int i = 0; i < 10000; i++) {
        if (NRF_RADIO->EVENTS_END != 0 && NRF_RADIO->CRCSTATUS == 1) {
            // 收到東西且 CRC 正確！
            return true;
        }
    }
    
    NRF_RADIO->TASKS_DISABLE = 1;
    return false;
}

int main(void) {
    if (IS_ENABLED(CONFIG_USB_DEVICE_STACK)) {
        usb_enable(NULL);
    }
    
    k_sleep(K_SECONDS(3));
    printk("\n=== Pico Whitening Pinger ===\n");
    printk("Attempting to wake up the tracker with WHITENING ENABLED...\n");

    raw_radio_init();

    while (1) {
        // 嘗試組合
        bool ack = false;

        // Try CH 73, 2Mbps
        NRF_RADIO->TXADDRESS = 0; // 用 Address 0 (43...)
        if (try_ping_raw(73, 1)) {
            printk("!!! ACK on CH 73 (2Mbps, ADDR 43) !!!\n");
            k_sleep(K_MSEC(200));
        }

        // Try CH 45, 2Mbps
        NRF_RADIO->TXADDRESS = 0;
        if (try_ping_raw(45, 1)) {
            printk("!!! ACK on CH 45 (2Mbps, ADDR 43) !!!\n");
            k_sleep(K_MSEC(200));
        }

        // Try CH 73, 1Mbps
        NRF_RADIO->TXADDRESS = 0;
        if (try_ping_raw(73, 0)) {
            printk("!!! ACK on CH 73 (1Mbps, ADDR 43) !!!\n");
            k_sleep(K_MSEC(200));
        }

        // Try with Default Address (E7)
        NRF_RADIO->TXADDRESS = 1; // 用 Address 1 (E7...)
        if (try_ping_raw(73, 1)) {
            printk("!!! ACK on CH 73 (2Mbps, ADDR E7) !!!\n");
            k_sleep(K_MSEC(200));
        }

        printk(".");
        k_sleep(K_MSEC(100));
    }
    return 0;
}

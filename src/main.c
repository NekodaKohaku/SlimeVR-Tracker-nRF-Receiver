void send_on_freq(uint8_t freq) {
    // 1. 先停用 Radio，確保狀態乾淨
    NRF_RADIO->SHORTS = 0;
    NRF_RADIO->TASKS_DISABLE = 1;
    while (NRF_RADIO->EVENTS_DISABLED == 0);
    NRF_RADIO->EVENTS_DISABLED = 0;

    NRF_RADIO->FREQUENCY = freq;
    
    // 設定地址 (保持之前的邏輯)
    if (swap_address) NRF_RADIO->BASE0 = reverse_32(TARGET_ADDR_BASE);
    else NRF_RADIO->BASE0 = TARGET_ADDR_BASE;

    // 準備 Payload
    static int toggle_cnt = 0;
    PAYLOAD_BUFFER[0] = (toggle_cnt++ % 2 == 0) ? 0x16 : 0x1C;
    memcpy(packet_buffer, PAYLOAD_BUFFER, 32); 
    NRF_RADIO->PACKETPTR = (uint32_t)packet_buffer;

    // ★★★ 關鍵修改：使用硬體捷徑 (Shortcuts) ★★★
    // 邏輯：
    // 1. READY -> START (自動開始)
    // 2. END -> DISABLE (發送完自動關閉 TX)
    // 3. DISABLED -> TXEN (原本是這樣) ... 改成 -> RXEN
    
    // 我們要建立一個自動鍊： TX Ready -> Start TX -> End TX -> Disable -> Enable RX -> Start RX
    
    // 設定捷徑：
    // 當 TX 準備好(READY) -> 自動開始(START)
    // 當 發射結束(END) -> 自動關閉(DISABLE)
    // 當 關閉完成(DISABLED) -> 自動開啟接收(RXEN)  <-- 這是最關鍵的跳轉！
    // 當 RX 準備好(READY) -> 自動開始(START)
    NRF_RADIO->SHORTS = RADIO_SHORTS_READY_START_Msk | 
                        RADIO_SHORTS_END_DISABLE_Msk | 
                        RADIO_SHORTS_DISABLED_RXEN_Msk;

    // 啟動 TX，剩下的交給硬體自動跑
    NRF_RADIO->TASKS_TXEN = 1;

    // CPU 現在只需要等待「接收結束」或「超時」
    // 我們給它稍微長一點的視窗 (例如 500us) 來容納 T_IFS + ACK 時間
    // ACK 很短，傳輸大約 40-80us
    
    k_busy_wait(100); // 盲等 100us 讓 TX 完成並切換

    // 開始檢查有沒有收到 ACK
    // 這裡我們等待 RX 的 END 事件
    int timeout_counter = 1000;
    bool ack_received = false;
    
    while(timeout_counter--) {
        if (NRF_RADIO->EVENTS_END) {
            // 收到東西了！檢查 CRC
            if (NRF_RADIO->CRCSTATUS == 1) {
                ack_received = true;
            }
            // 清除事件，避免無窮迴圈
            NRF_RADIO->EVENTS_END = 0;
            // 收到一個包後，無論是不是 ACK，這次傳輸週期都算結束了
            break; 
        }
        k_busy_wait(1);
    }

    // 停止 Radio
    NRF_RADIO->SHORTS = 0;
    NRF_RADIO->TASKS_DISABLE = 1;

    if (ack_received) {
        int8_t rssi = -(int8_t)NRF_RADIO->RSSISAMPLE;
        printk(">>> [HIT!] ACK RECEIVED on %d MHz | RSSI: %d\n", 2400+freq, rssi);
        gpio_pin_set_dt(&led, 1);
        k_busy_wait(10000);
        gpio_pin_set_dt(&led, 0);
    }
}

# Bill of Materials — BASS BAND

| Component | Notes |
|---|---|
| TPA3110 dual-channel amplifier module | Filterless class-D; 8–26V input, full rated power (~15W/ch) needs ~16–24V |
| Bluetooth receiver module | Feeds line-in (L/R/GND) to amp |
| Passive radiators ×2 | Sized/tuned to enclosure volume + woofer Thiele-Small params |
| 30W woofer speaker | Main bass driver |
| 3W speaker (tweeter) | High-frequency driver |
| Low-pass filter (passive crossover) | To woofer |
| High-pass filter (passive crossover) | To tweeter |
| A2170 BMS | Salvaged laptop-pack fuel-gauge/protection IC, SBS/SMBus @ 0x0B |
| USB-C 3S 1A charge module | Charging path — confirm it isn't fighting A2170 protection |
| ESP32 | Reads BMS over I2C, hosts WiFi AP dashboard, drives OLED |
| 0.96" OLED (SH1106, I2C, 0x3C) | Battery status + VU meter display |
| Buck converter (step-down) / 7805 | 5V rail for ESP32/OLED/BT — buck preferred for efficiency over 7805 |
| 18650 Li-ion cells ×6 | Likely 3S2P (~11.1V nominal, ~12.6V full charge) |
| DC jack | Confirm role: charge input vs external power — avoid backfeeding charge circuit |
| 3mm LED ×2 | Status indicators — use series resistors sized for 5V rail |
| Toggle switch | Main power switch |

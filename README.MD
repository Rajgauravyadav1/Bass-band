# BASS BAND

A DIY battery-powered Bluetooth speaker with a TPA3110 dual-channel amp,
30W woofer + 3W tweeter, passive radiators, and an ESP32-driven OLED
status display / WiFi dashboard for battery monitoring.

**Repo:** https://github.com/Rajgauravyadav1/esp32-a2170-battery-monitor


## Features
- Bluetooth audio input to a filterless TPA3110 class-D amplifier
- 6×18650 Li-ion pack (3S2P) with laptop-salvaged A2170 BMS (SMBus/SBS @ 0x0B)
- ESP32 reads voltage, current, SOC, temperature, and charge/discharge
  status directly from the BMS over I2C
- 0.96" OLED (SH1106) shows an animated VU meter screen and a battery
  data screen, switching automatically every 30s
- ESP32 hosts a WiFi access point (`TECHHI` / `12341234`) serving a live
  web dashboard (`/` and `/data` JSON endpoint) for battery stats

## Repo structure
```
BASS BAND/
├── firmware/
│   └── BASS BAND.ino     # ESP32 sketch
├── docs/
│   └── BOM.md            # Full parts list
└── README.MD
```

## Firmware dependencies
Install via Arduino Library Manager:
- `Wire` (built-in)
- `WiFi` (built-in, ESP32 core)
- `ESPAsyncWebServer`
- `AsyncTCP`
- `Adafruit_GFX`
- `Adafruit_SH110X`

## Hardware notes
See [`docs/BOM.md`](docs/BOM.md) for the full parts list.

## Wiring quick reference
- I2C: SDA → GPIO21, SCL → GPIO22
- BMS SMBus address: `0x0B`
- OLED I2C address: `0x3C`

## License
MIT — see [`LICENSE`](LICENSE).

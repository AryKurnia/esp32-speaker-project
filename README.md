# 🔊 ESP32 Bluetooth Speaker with DSP Crossover

Speaker Bluetooth DIY berbasis ESP32 dengan DSP crossover digital, DAC PCM5102A, dan amplifier PAM8610. Terinspirasi dari speaker premium Harman Kardon Giga Genie 2.

---

## ✨ Fitur

- **Bluetooth A2DP Sink** — terima audio dari HP/laptop via Bluetooth
- **DSP Crossover Linkwitz-Riley 24dB/octave** — pemisah frekuensi digital
  - Channel L → Low Pass Filter → Woofer (bass)
  - Channel R → High Pass Filter → Tweeter (treble)
- **Mono Mixing** — stereo L+R digabung sebelum diproses
- **Connection Chime** — suara notifikasi saat BT konek/disconnect (via LittleFS)
- **Serial Controller** — kontrol parameter audio realtime via Serial Monitor
- **Persistent Settings** — simpan konfigurasi ke flash (NVS) agar tidak hilang saat restart
- **LED Indicator** — indikator status koneksi Bluetooth

---

## 🔧 Hardware

| Komponen | Spesifikasi | Fungsi |
|---|---|---|
| ESP32 DOIT DEVKIT V1 | CP2102, 38 Pin | Mikrokontroler utama |
| PCM5102A DAC | 24-bit, 192kHz, I2S | Digital to Analog Converter |
| PAM8610 Amplifier | 2x10W, Class D | Amplifier audio |
| Power Adapter | 12V 5A | Sumber daya utama |
| LM2596 Buck Converter | Step-down ke 5V | Power ESP32 |

---

## 📌 Wiring

### ESP32 → PCM5102A (I2S)

| ESP32 | PCM5102A | Keterangan |
|---|---|---|
| GPIO25 | BCK | Bit Clock |
| GPIO26 | LCK | Word Select |
| GPIO22 | DIN | Data |
| 3.3V | VIN | Power |
| 3.3V | Pin 3 (XSMT) | Unmute |
| GND | GND | Ground |
| GND | SCK | Internal PLL |
| GND | Pin 1 (FLT) | Normal latency |
| GND | Pin 2 (DEMP) | De-emphasis off |
| GND | Pin 4 (FMT) | I2S standard |

### PCM5102A → PAM8610

| PCM5102A | PAM8610 |
|---|---|
| LOUT (Pin L) | Input L |
| ROUT (Pin R) | Input R |
| AGND (Pin G) | GND |

### PAM8610 → Speaker

| PAM8610 | Speaker |
|---|---|
| OUT L+ / OUT L- | Woofer |
| OUT R+ / [2.2µF MKP] | Tweeter |

### Power

```
Adaptor 12V 5A
├── PAM8610 VCC (12V langsung)
└── LM2596 (step-down 5V)
         └── ESP32
```

---

## 📁 Struktur Project

```
esp32-speaker/
├── src/
│   └── main.cpp
├── data/
│   ├── connected.wav      ← chime BT konek
│   └── disconnected.wav   ← chime BT disconnect
├── platformio.ini
├── .gitignore
└── README.md
```

---

## 🚀 Build & Upload

### Menggunakan PlatformIO (VS Code)

```bash
# Build
Ctrl+Alt+B

# Upload filesystem (LittleFS wav files) — lakukan sekali
PlatformIO → Project Tasks → Platform → Upload Filesystem Image

# Upload firmware
Ctrl+Alt+U

# Serial Monitor
Ctrl+Alt+M
```

### platformio.ini

```ini
[env:esp32doit-devkit-v1]
platform = espressif32
board = esp32doit-devkit-v1
framework = arduino
monitor_speed = 115200
upload_speed = 921600

lib_deps =
    https://github.com/pschatzmann/ESP32-A2DP.git

board_build.filesystem = littlefs
```

---

## 🎛️ Serial Controller

Buka Serial Monitor (115200 baud) dan ketik perintah:

| Perintah | Contoh | Keterangan |
|---|---|---|
| `freq:<Hz>` | `freq:5000` | Ubah crossover point (100-20000 Hz) |
| `gain_w:<val>` | `gain_w:1.2` | Gain woofer |
| `gain_t:<val>` | `gain_t:0.8` | Gain tweeter |
| `wav_vol:<val>` | `wav_vol:0.7` | Volume chime WAV (0.0-1.0) |
| `save` | `save` | Simpan settings ke flash |
| `reset` | `reset` | Reset ke default |
| `status` | `status` | Lihat settings saat ini |
| `help` | `help` | Tampilkan daftar perintah |

---

## ⚙️ Default Settings

| Parameter | Default | Keterangan |
|---|---|---|
| Crossover | 5000 Hz | Titik pemisah woofer/tweeter |
| Gain Woofer | 1.0 | Normal |
| Gain Tweeter | 1.0 | Normal |
| WAV Volume | 0.7 | 70% |

---

## 🏗️ Arsitektur DSP

```
Bluetooth A2DP (stereo)
        │
   Mono Mixing
   (L + R) / 2
        │
   ┌────┴────┐
   │         │
  LPF       HPF       ← Stage 1 (Butterworth Q=0.7071)
   │         │
  LPF       HPF       ← Stage 2 (cascade = Linkwitz-Riley 24dB/oct)
   │         │
Woofer    Tweeter
(CH L)    (CH R)
```

---

## 📚 Referensi

- [ESP32-A2DP Library](https://github.com/pschatzmann/ESP32-A2DP)
- [PCM5102A Datasheet](https://www.ti.com/product/PCM5102A)
- [Elliott Sound Products — Active Crossover](https://sound-au.com/project09.htm)
- [Linkwitz-Riley Crossover Theory](https://en.wikipedia.org/wiki/Linkwitz%E2%80%93Riley_filter)

---

## 📋 Roadmap

- [x] Bluetooth A2DP sink
- [x] I2S DAC output (PCM5102A)
- [x] Mono mixing
- [x] DSP crossover (Linkwitz-Riley 24dB/oct)
- [x] Connection chime (LittleFS WAV)
- [x] Serial controller
- [x] Persistent settings (NVS)
- [ ] Rotary encoder controller
- [ ] OLED display
- [ ] WiFi streaming (Navidrome/Subsonic)
- [ ] OTA firmware update
- [ ] Volume control

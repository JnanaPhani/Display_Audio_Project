# Fleet Audio Board Firmware

This firmware runs on the **ESP32-S3-powered Audio Board**. It receives barcode scans from the **Smart Token Display** via an offline-resilient ESP-NOW connection and announces token numbers sequentially using a **DFPlayer Mini** MP3 module.

---

## 🔌 Hardware Wiring & Pinout

The ESP32-S3 communicates with the DFPlayer Mini via UART2.

| ESP32-S3 Pin | DFPlayer Pin | Description | Note |
|---|---|---|---|
| **5V / VBUS** | Pin 1 (VCC) | Power Supply | 5V is highly recommended over 3.3V to prevent card brownouts. |
| **GPIO 7 (TX)** | Pin 2 (RX) | UART TX $\rightarrow$ DFPlayer RX | **Requires a 1K $\Omega$ resistor in series** to reduce logic level noise. |
| **GPIO 6 (RX)** | Pin 3 (TX) | UART RX $\leftarrow$ DFPlayer TX | Directly connected. |
| **GND** | Pin 7/10 (GND)| Common Ground | Connect to ESP32 Ground. |
| **SPK1 / SPK2**| Pin 6 & Pin 8 | Audio Output | Connect directly to a 2W-5W (8 $\Omega$) speaker. |

---

## 📂 microSD Card Folder & File Layout

The DFPlayer Mini uses a simplified file system processor and requires a strict FAT16 or FAT32 format with files organized as follows:

```text
microSD Root/
├── 01/                     <-- System Prompts Folder
│   ├── 001.mp3             <-- Prefix track ("Token number...")
│   └── 002.mp3             <-- Suffix track ("Please collect your order...")
├── 02/                     <-- Direct Numbers Folder (1 to 99)
│   ├── 001.mp3             <-- "One"
│   ├── 002.mp3             <-- "Two"
│   └── 099.mp3             <-- "Ninety Nine"
└── 03/                     <-- Fallback Digits Folder (0 to 9)
    ├── 001.mp3             <-- "Zero"
    ├── 002.mp3             <-- "One"
    └── 010.mp3             <-- "Nine"
```

> [!WARNING]
> * Files must start with a **3-digit number** (e.g. `001.mp3`, `089_eighty_nine.mp3`).
> * Folders must be exactly **two digits** (`01`, `02`, `03`).
> * Remove all hidden operating system files (e.g. `._*` or `.DS_Store` created by macOS/Linux indexing) as they will confuse the DFPlayer and cause `cannot play file (cannot find file, code 6)` errors.

---

## ⚙️ Architecture & Boot Flow

The Audio Board uses a robust, dual-core **offline-first** network architecture:

1. **Non-Blocking Boot:** Network tasks are split. The Wi-Fi interface initializes (`wifi_start()`) and immediately brings up ESP-NOW, pinning the radio to **Channel 1**.
2. **Offline Mode:** The board is instantly ready to receive scanned token broadcasts from the Display Board, even if the local Wi-Fi router is offline or the board is unprovisioned.
3. **Online Mode (Provisioning & Supabase):** A background network task runs parallel on CPU 0 to handle Wi-Fi connections, SNTP time synchronization, and Supabase WebSocket real-time heartbeats.
4. **Coexistence Channel Hopping:** If the Audio Board connects to an AP on another channel (e.g. Channel 7), the Display Board uses channel hopping (cycling channels 1–11) to deliver ESP-NOW payloads without requiring the Display Board to connect to the router.

---

## 🚀 Building & Flashing

This project uses **ESP-IDF v5.4**.

### 1. Set Build Target
```bash
idf.py set-target esp32s3
```

### 2. Compile Firmware
```bash
idf.py build
```

### 3. Flash with Secure Boot Key
The system is configured with hardware secure boot. Use the `flash_signed.sh` helper script to sign the compiled binary with your PEM key and flash it to the device:
```bash
# Flash to a specific serial port (e.g., /dev/ttyUSB1)
chmod +x flash_signed.sh
./flash_signed.sh --full -p /dev/ttyUSB1
```

---

## 🔊 Volume Control

The Audio Board maps Supabase Remote Brightness configurations to the speaker volume:
* Volume updates are synchronized in real-time.
* A brightness value of **`1%` to `100%`** from the dashboard is automatically scaled to the DFPlayer volume range **`0` to `30`**.

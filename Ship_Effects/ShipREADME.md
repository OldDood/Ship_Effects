
### 🚢 ShipEffects S3 v1.1: Audio & Logic Engine

A high-performance automation and telemetry engine for a ship model display, running on the **ESP32-S3**. This version marks the successful transition from Camera functionality to a dedicated **I2S Digital Audio** and **Solar-aware** control system.

---

## 📦 Component List (BOM)

### Core Hardware
* **Microcontroller:** Freenove ESP32-S3-WROOM (Camera Module variant).
* **Audio Amplifier:** MAX98357A I2S Class-D Mono Amp.
* **Storage:** Micro SD Card (formatted to FAT32, up to 32GB supported).
* **Speaker:** 4Ω or 8Ω nominal impedance (connected to MAX98357A output).

### Software Dependencies (ESP-IDF)
To compile this project, the following components must be declared in your `main/CMakeLists.txt`:
* `esp_driver_i2s`: Required for the v5.3+ I2S Standard Driver.
* `esp_camera`: Necessary to manage the S3-CAM pin initialization.
* `fatfs` & `esp_vfs`: For the Virtual File System and SD card management.
* `sdmmc`: For the 1-bit high-stability SD interface.

---

## 🛠 Hardware Pinout (Confirmed v1.1)

| Component | GPIO Pin | Configuration / Notes |
| :--- | :--- | :--- |
| **SDMMC CLK** | `39` | 1-bit mode. Drive strength: `GPIO_DRIVE_CAP_3`. |
| **SDMMC CMD** | `38` | 1-bit mode. Drive strength: `GPIO_DRIVE_CAP_3`. |
| **SDMMC D0** | `40` | 1-bit mode. Drive strength: `GPIO_DRIVE_CAP_3`. |
| **I2S BCLK** | `7` | **Bit Clock.** Measured at 1.64V (Active). |
| **I2S LRC** | `15` | **Word Select.** Measured at 1.64V (Active). |
| **I2S DIN** | `6` | **Data Out.** Digital audio stream to MAX98357A. |
| **AMP SD** | `4` | **Shutdown.** Measured at 3V (Amp Active). |
| **AMP GAIN** | `5` | **Gain.** Measured at 3.165V (**6dB** safety floor). |

---

## ☀️ Background Solar Engine
* **Location:** Adelaide/Modbury, South Australia ($34.9285^\circ$ S).
* **Logic:** Calculates daily sunrise/sunset to drive autonomous ship lighting.
* **Night Detection:** Uses a **15-minute "True Dark" buffer** post-sunset.

---

## 🌐 Web Management Portal
* **Function:** Real-time MP3 upload with XHR progress tracking.
* **Storage Telemetry:** Displays remaining SD capacity (MB) vs. total volume.
* **File Control:** Integrated Play, Delete, and Bulk Wipe utilities.

---

## 🚀 Execution Flow (`app_main`)
1.  **NVS Init:** Prepares flash for WiFi credentials.
2.  **Audio Setup:** Initializes MAX98357A pins (SD and Gain).
3.  **Digital Bus:** Mounts SD Card and starts I2S Clocks (GPIO 7 & 15).
4.  **Network:** Connects WiFi and synchronizes time via SNTP.
5.  **Persistence:** Restores wall-clock from RTC memory if waking from sleep.

---

## 💾 Persistent RTC Memory Map
* `last_success_time_sec`: Tracks timing for recurring effects.
* `sleep_anchor_time`: The master `timeval` reference.
* `time_is_set`: Boolean flag for valid time restoration.

--


### 🚢 ShipEffects S3 v1.1: Audio & Logic Engine

A high-performance automation and telemetry engine for a ship model display, running on the **ESP32-S3-WROOM**. This version marks the successful functionality test of a dedicated **I2S Digital Audio** and **Solar-aware** control system.

---
**This version can play any .MP3 file and is a good working base for future additions.**

## 📦 Component List (BOM)

### Core Hardware
* **Microcontroller:** Freenove ESP32-S3-WROOM (Camera Module variant with on board SD Card Reader). The camera is not used in this project.
* **Audio Amplifier:** MAX98357A I2S Class-D Mono Amp.
* **Storage:** Micro SD Card (formatted to FAT32, up to 32GB supported).
* **Speaker:** 4Ω or 8Ω nominal impedance (connected to MAX98357A output).

### Software Dependencies (ESP-IDF v5.3+)
To compile this project, the following components must be declared in your `main/CMakeLists.txt`:
* `esp_driver_i2s`: Required for the I2S Standard Driver.
* `fatfs` & `esp_vfs`: For the Virtual File System and SD card management.
* `sdmmc`: For the 1-bit high-stability SD interface.
* `esp_http_server`: Powers the remote management portal.

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
| **AMP GAIN** | `5` | **Gain.** Measured at 3.165V (**12dB** standard floor). |

---

## ⚙️ Project Configuration (`menuconfig`)
The project now utilizes `Kconfig.projbuild` for rapid field adjustments without code changes:
* **WiFi Credentials:** SSID and Password managed via the UI.
* **Audio Mode Selector:** 1. **Internal (1):** Diagnostic Sine Sweep (300Hz–2400Hz).
    2. **WAV (2):** Diagnostic SD Card Playback (e.g., `ship_horn.wav`).
    3. **MP3 (3):** Compressed storage playback. (This is normal operating mode)

---

## 🧪 Audio Execution Modes
### 1. Diagnostic Suite: `test_speaker_sine_repeater`
Verifies audio path integrity and SNR:
* **Waveform:** Soft-start Sine Wave (300Hz, 600Hz, 1200Hz, 2400Hz).
* **Pattern:** 2-second steps per frequency.
* **Silence:** 15-second "Silent Sleep" between 4 major cycles.

### 2. High-Fidelity Playback: `play_wav_file` / `MP3`
Streams audio from the SD card:
* **Format:** 16-bit PCM WAV (44.1kHz / Mono optimized) or MP3.
* **Automation:** Configured to loop **4 times** with **15-second intervals**.
* **Clean-Up:** Calls `i2s_channel_disable()` during intervals to ensure zero-hiss silence.
* **Stability Fix:** Task is now pinned to **Core 1** to avoid Core 0 Watchdog Timeouts during WiFi activity.

---

## 🚀 Execution Flow (`app_main`)
1.  **NVS Init:** Prepares flash for WiFi credentials.
2.  **System Event Group:** Creates `s_system_event_group` to sync network/audio states.
3.  **Peripheral Warm-up:** Initializes SD Card (**GPIO 38-40**) and Amplifier (**GPIO 4-5**) hardware.
4.  **Digital Bus:** Starts I2S Clocks (**GPIO 7 & 15**) and attaches the driver.
5.  **Network & Time:** Connects WiFi (with `WIFI_PS_NONE` for stability) and synchronizes ACST time via SNTP.
6.  **Web Portal:** Launches the management interface for remote telemetry and file control.
7.  **Audio Task Launch:** Pinned to **Core 1** with a 10s wait for WiFi/Time sync to ensure DMA stability.

---

## ☀️ Background Solar Engine
* **Location:** Adelaide/Modbury, South Australia ($34.9285^\circ$ S).
* **Logic:** Calculates daily sunrise/sunset to drive autonomous ship lighting.
* **Night Detection:** Uses a **15-minute "True Dark" buffer** post-sunset for "Lights On" triggers.

---

## 💾 Persistent RTC Memory Map
* `last_success_time_sec`: Tracks timing for recurring effects across light sleeps.
* `sleep_anchor_time`: The master `timeval` reference for clock restoration.
* `time_is_set`: Boolean flag for valid time restoration after wakeup.

---
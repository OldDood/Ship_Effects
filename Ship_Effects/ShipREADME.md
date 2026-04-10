### 🚢 ShipEffects S3 v1.2: Audio & Logic Engine

A high-performance automation and telemetry engine for a ship model display, running on the **ESP32-S3-WROOM**. This version marks the successful functionality test of a dedicated **I2S Digital Audio** and **Solar-aware** control system.

---
**Status: STABLE | Core: ESP-IDF v5.3.1 | Hardware: ESP32-S3**

**This version marks the transition from prototype to a robust automation engine.**
**Major focus on filesystem hygiene, resource stability, and binary logic synchronization.**
**This version has been fully tested**
**Need to add some minor enhancements to the web interface in future**
**Need to do some changes to the code that plays the Audio.wav file, it seems like it is playing at the wrong speed now**

## 📦 Component List (BOM)

### Core Hardware
* **Microcontroller:** Freenove ESP32-S3-WROOM (Camera Module variant with on board SD Card Reader). The camera is not used in this project.
* **Audio Amplifier:** MAX98357A I2S Class-D Mono Amp.
* **Storage:** Micro SD Card (formatted to FAT32, up to 32GB supported).
* **Speaker:** 4Ω or 8Ω nominal impedance (connected to MAX98357A output).

### Software Dependencies (ESP-IDF v5.3+)
**To compile this project, the following components must be declared in your `main/CMakeLists.txt`:**
* `esp_driver_i2s`: Required for the I2S Standard Driver..cpp to main.cpp
* `fatfs` & `esp_vfs`: For the Virtual File System and SD card management.
* `sdmmc`: For the 1-bit high-stability SD interface.
* `esp_http_server`: Powers the remote management portal.

---

It’s a good idea to keep the documentation in sync with the physical reality of the workbench. I've updated the table to include your weighted WLED outputs and refined the notes to reflect the latest measurements and configurations.

## 🛠 Hardware Pinout (Confirmed v1.2)

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
| **WLED_OP_BIT1** | `10` | **Parallel Bus Bit 0.** Weighted value: 1. |
| **WLED_OP_BIT2** | `11` | **Parallel Bus Bit 1.** Weighted value: 2. |
| **WLED_OP_BIT4** | `12` | **Parallel Bus Bit 2.** Weighted value: 4. |
| **WLED_OP_BIT8** | `13` | **Parallel Bus Bit 3.** Weighted value: 8. |

---

### Integration Notes:
* **WLED Interface:** The 4-bit bus provides 15 unique trigger IDs (plus 0 for idle).
* **Safety Logic:** All WLED output pins are initialized as push-pull outputs to ensure clean logic transitions for the receiver.
* **Audio Sync:** GPIOs 10-13 are updated via the marker parser during I2S playback.

Once you commit this, your README will be a perfect "as-built" record of the hardware! Ready to start on that `M.S.CS` parser logic?

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

## 🎼 MP3 Decoder & Sync Engine
The system utilizes a dedicated decoder task to bridge the gap between compressed SD data and the I2S DMA buffers.

**Decoding:** Software-based MP3 decoding (libmad/minimp3) streaming to i2s_channel_write.

**Buffer Management:** Implements a 4KB ring buffer to prevent "under-run" stutters during SD card latency spikes.
---
## 💡 WLED Parallel Interface
To trigger lighting effects without the overhead of network packets, the S3 communicates via a **4-bit physical parallel bus** to the WLED controller.

| Bit | GPIO | Weight | Function |
| :--- | :--- | :--- | :--- |
| **0** | `10` | 1 | **LSB** - Effect Trigger |
| **1** | `11` | 2 | Effect Trigger |
| **2** | `12` | 4 | Effect Trigger |
| **3** | `13` | 8 | **MSB** - Effect Trigger |

* **Logic Level:** 3.3V CMOS.
* **Protocol:** The S3 sets the 4-bit state → WLED reads the integer value (0–15) → WLED executes the corresponding Preset or Segment effect.
* **Timing:** Bit states are held for the duration of the audio event to prevent ghost triggers.
* **Safety:** Pins are initialized as push-pull outputs to ensure clean logic transitions.

## 🚀 Execution Flow (`app_main`)
1.  **NVS Init:** Prepares flash for WiFi credentials.
2.  **System Event Group:** Creates `s_system_event_group` to sync network/audio states.
3.  **Peripheral Warm-up:** Initializes SD Card (**GPIO 38-40**) and Amplifier (**GPIO 4-5**) hardware.
4.  **Digital Bus:** Starts I2S Clocks (**GPIO 7 & 15**) and attaches the driver.
5.  **Network & Time:** Connects WiFi (with `WIFI_PS_NONE` for stability) and synchronizes ACST time via SNTP.
6.  **Web Portal:** Launches the management interface for remote telemetry and file control.
7.  **Audio Task Launch:** Pinned to **Core 1** with a 10s wait for WiFi/Time sync to ensure DMA stability.

---
## 🆕 New in v1.2 (Commit Changes)
**Naming Police (Web Guard): Added client-side JavaScript validation to the Web Portal. Prevents filenames with spaces or special characters from reaching the SD card, eliminating URL-encoding traps (%20).**

**Resource Expansion: Increased .max_files from 5 to 10 in the FatFS configuration to allow concurrent access by the Web Server and the Audio/CSV Task.**

**Core Affinity: Audio Task is strictly pinned to Core 1 to prevent Watchdog (WDT) triggers during high WiFi traffic on Core 0.**

**Enhanced Autoplay: Implemented a non-blocking autoplay.txt reader that initializes exactly 10 seconds post-boot to ensure SNTP time sync is complete.**

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
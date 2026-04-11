### 🚢 ShipEffects S3 v1.3: Audio & Logic Engine

A high-performance automation and telemetry engine for a ship model display, running on the **ESP32-S3-WROOM**. This version marks the successful functionality test of a dedicated **I2S Digital Audio**

---
**Status: STABLE | Core: ESP-IDF v5.3.1 | Hardware: ESP32-S3**
**This version is fully functional and tested**

## Critical Integration Note:

**WLED Controller: This project requires WLED v0.15.0-b7 "Full" (or later). Standard or "gcc" builds often lack the Serial JSON API required to trigger ship effects.**

**Communication: Legacy 4-bit parallel digital outputs have been deprecated and removed. Control is now handled via a 2-wire Serial JSON Bridge.**

## 📦 Component List (BOM)

### Core Hardware
* **Microcontroller:** This projects software target - Freenove ESP32-S3-WROOM (Camera Module variant with on board SD Card Reader). The camera is not used in this project.
* **Audio Amplifier:** MAX98357A I2S Class-D Mono Amp.
* **Storage:** Micro SD Card (formatted to FAT32, up to 32GB supported).
* **Speaker:** 4Ω or 8Ω nominal impedance (connected to MAX98357A output).
* **ESP32 configured with binary file WLED v0.15.0-b7 "Full"** -This is not the processor for this projects software, it is **external** hardware

### Software Dependencies (ESP-IDF v5.3+)
**To compile this project, the following components must be declared in your `main/CMakeLists.txt`:**
* `esp_driver_i2s`: Required for the I2S Standard Driver..cpp to main.cpp
* `fatfs` & `esp_vfs`: For the Virtual File System and SD card management.
* `sdmmc`: For the 1-bit high-stability SD interface.
* `esp_http_server`: Powers the remote management portal.
* `esp_wifi`: Manages the physical radio and 802.11 stack; handles the connection to your Access Point and maintains the link for the Web Portal.
* `esp_event`: The system's "Post Office"; allows different parts of your code (like WiFi and the Web Server) to communicate via background signals without clashing.
* `nvs_flash`: (Non-Volatile Storage): A digital notebook in the flash memory that saves your WiFi credentials and system settings so they aren't lost when the power is turned off.
* `esp_driver_uart`:The dedicated hardware controller for serial communication; manages the timing and memory buffers for the JSON command link to WLED.

---

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
| **UART_NUM_1 TX** | `17` | T**X serial pin to WLED Controller** |
| **UART_NUM_1 RX** | `18` | **RX serial pin from WLED Controller**|
| **AUTOPLAY SW** | `14` | **Master Toggle.** Internal Pull-up enabled. |

---

### Integration Notes:
* **WLED Interface:** The 4-bit bus provides 15 unique trigger IDs via serial interface to WLED(plus 0 for idle).
* **Safety Logic:** All WLED output pins are initialized as push-pull outputs to ensure clean logic transitions for the receiver.
* **Audio Sync:** GPIOs 10-13 are updated via the marker parser during I2S playback.

## ⚙️ Project Configuration (`menuconfig`)
The project now utilizes `Kconfig.projbuild` for rapid field adjustments without code changes:
* **WiFi Credentials:** SSID and Password managed via the UI.
* Search for "ship" in menuconfig for mode selector
 **Audio Mode Selector:** 1. **Internal (1):** Diagnostic Sine Sweep (300Hz–2400Hz).
    2. **WAV (2):** Diagnostic SD Card Playback (e.g., `ship_horn.wav`).
    3. **MP3 (3):** Compressed storage playback. (This is normal operating mode)

---
## 📂 Mandatory SDCard Files
To ensure the ShipEffects S3 engine initializes correctly and synchronizes with the WLED controller, the following files must be present in the root directory of the SD card (Formatted to FAT32).

**1. autoplay.txt**
This file acts as the master script for the ship's autonomous behavior.

Purpose: Contains the filename of the track to be played automatically 10 seconds after boot.

Format: Plain text containing only the filename (e.g., storm_sequence.mp3).

**2. ship_horn.wav (or designated MP3)**
Purpose: Standard diagnostic and system sound file for mode 2.

**3. i2S Test.mp3 and TestAudio\i2S Test.csv**
Purpose: Auto play files for mode 3 if sutoplay.txt contains the line - TestAudio\i2S Test.csv

These file are stored for backup in the ESP_IDF project directory "Ship_Effects"
**SDCard required files**
TestAudio\autoplay.txt
TestAudio\TestAudio\i2S Test.csv.csv
TestAudio\i2S Test.mp3
TestAudio\ship_horn.wav
**Backup for WLED presets**
TestAudio\wled_presets.json
**Configuration file for serial testing program for WLED serial interface using the program "Yat"** https://sourceforge.net/projects/y-a-terminal/
TestAudio\Ship_Project.yat

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
## 🛰️ Serial bus to WLED controller

**1. WLED JSON Serial Bridge**
**Protocol: Asynchronous Serial (UART)**

**Baud Rate: 115200**

**Data Format: JSON (e.g., {"ps": 1})**

**Hardware Path: GPIO 17 (TX) -> WLED RX | GPIO 18 (RX) (Reserved)**

## 🚀 Execution Flow (`app_main`)
1.  **NVS Init:** Prepares flash for WiFi credentials.
2.  **System Event Group:** Creates `s_system_event_group` to sync network/audio states.
3.  **Peripheral Warm-up:** Initializes SD Card (**GPIO 38-40**) and Amplifier (**GPIO 4-5**) hardware.
4.  **Digital Bus:** Starts I2S Clocks (**GPIO 7 & 15**) and attaches the driver.
5.  **Network & Time:** Connects WiFi (with `WIFI_PS_NONE` for stability) and synchronizes ACST time via SNTP.
6.  **Web Portal:** Launches the management interface for remote telemetry and file control.
7.  **Audio Task Launch:** Pinned to **Core 1** with a 10s wait for WiFi/Time sync to ensure DMA stability.

---
## 🛰️ WLED Serial JSON Bridge (v1.3)Protocol: Asynchronous Serial (UART)Baud Rate: 115200 
Format: JSON-encapsulated commands.Operational Logic: The S3 Master Engine parses markers within the MP3 stream. Upon hitting a marker, a JSON command message sent via UART1 to the WLED controller. These commands control the presets on the WLED controller. Other commands can be added if required by modifying send_wled_command(uint8_t marker_id) function in main.cpp

## 📡 WLED Control Command  Example
**To trigger a specific preset (e.g., Preset 1), the Master S3 sends the following JSON object over UART:**
{"ps": 1}
**Field Definition:**
**ps (Preset): The ID of the preset to activate. This corresponds to preset ID1 in the WLED web interface.**

## 🕹️ Physical On and off switch on GPIO14
**"Master Override" to enable or disable the automatic play of the .MP3 listed in autoplay.txt on the located on theSD card.**
example text - i2S_Test - will play I2S_Test.mp3 located on the SD Card


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
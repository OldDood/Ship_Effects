### 🚢 ShipEffects S3 v1.3: Audio & Logic Engine

A high-performance automation and telemetry engine for a ship model display, running on the **ESP32-S3-WROOM**. This version marks the successful functionality test of a dedicated **I2S Digital Audio** and **Solar-aware** control system.

---
**Status: STABLE | Core: ESP-IDF v5.3.1 | Hardware: ESP32-S3**

**This version addition-**
**WLED digital outputs have been removed as WLED controller does not support decoding**
**A serial connection on UART1 that issues WLED JSON commands**

**ToDo:- The serial bus interface to the WLED Controller needs verifying**

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
## 🆕 New in v1.2 (Commit Changes)
**Naming Police (Web Guard): Added client-side JavaScript validation to the Web Portal. Prevents filenames with spaces or special characters from reaching the SD card, eliminating URL-encoding traps (%20).**

**Resource Expansion: Increased .max_files from 5 to 10 in the FatFS configuration to allow concurrent access by the Web Server and the Audio/CSV Task.**

**Core Affinity: Audio Task is strictly pinned to Core 1 to prevent Watchdog (WDT) triggers during high WiFi traffic on Core 0.**

**Enhanced Autoplay: Implemented a non-blocking autoplay.txt reader that initializes exactly 10 seconds post-boot to ensure SNTP time sync is complete.**

## 🕹️ Physical Control & Safety Logic
**Version 1.2 introduces a hardware-based "Master Override" to manage the ship's autonomous behavior without needing a network connection.**

**1. Autoplay Toggle Switch**
**A physical toggle switch is monitored via a background polling task on Core 0.**

**GPIO Pin: 14 (Configured with Internal Pull-up).**

**ARMED (High/1): The system periodically checks the SD card for autoplay.txt and initiates playback sequences.**

**DISARMED (Low/0): All autonomous triggers are inhibited.**

**2. Instant-Kill Functionality**
**To ensure the ship can be silenced immediately, the audio decoding loop performs a "Live-Check" on the hardware state.**

**Logic: If the switch is flipped to OFF mid-track, the I2S stream is truncated, memory buffers are freed, and the file is closed within milliseconds.**

**Safety: This prevents "Zombie Tasks" from running in the background when the user expects silence.**

**3. Concurrency Protection (Busy Semaphore)**
**To prevent the system from "stepping on its own toes," a global Volatile Boolean (is_audio_playing) acts as a digital gatekeeper.**

**Operation: The system will refuse any new playback commands (Web or Autoplay) if the Busy Flag is active.**

**Recovery: The flag is automatically reset to false upon natural track completion or a hardware Kill-Switch event.**

**📊 System State Logging (Diagnostic Output)**
**The console output has been optimized to report state changes only when they occur, preventing log-spam while maintaining visibility:**

**Log Message	Trigger Condition**
AUTO: Switch flipped to ON (Armed)	Detected transition from 0 to 1 (or at Boot).
AUTO: Switch flipped to OFF (Disarmed)	Detected transition from 1 to 0.
AUDIO: Switch DISARMED: Killing playback.	Switch flipped to OFF while is_audio_playing was true.
BOOT: Auto-playing: [Filename]	Valid autoplay.txt entry found and system is Idle.

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
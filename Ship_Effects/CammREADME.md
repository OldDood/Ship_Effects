# 🌲 TrailCam S3 v1.0

A high-performance, ESP32-S3 based trail camera featuring Light Sleep optimization, RTC time persistence, and a web-based maintenance portal.

---

## 📝 Deployment Steps

1. **Physical Setup:** - Ensure the **Mode Switch** is set to **Maintenance** (High).
* Insert a FAT32-formatted microSD card (up to 32GB recommended for 1-bit mode stability).


2. **Initial Sync:** - Power on the device. Connect to the local WiFi.
* Wait for the Web Portal to indicate "Time Synced" (Status: Year 2026).


3. **Deployment:** - Flip the switch to **Field Mode** (Low).
* The device will perform a "Final Teardown" of WiFi resources and enter Light Sleep.
* Mount the camera. It is now armed for PIR triggers.


4. **Web Interface:** - Flip the switch to **Maintenance Mode** (High). This disconnects the internal 4 AAA Battery pack.
* Power the device via the USB interface.
* This enables the WIFI and Web Interface on the local house network (10.0.0.45).
* Open a Web Browser at the address above to manage photos stored on the device's SD card.



## 🛠 Hardware Pinout (Freenove ESP32-S3)

## 🛠 Hardware Power Supply (Buck Convertor 4.5 to 20VDC in. 5VDC Out (Adjustable))

## 🛠 Hardware Schematic (Trail Camera.kicad_sch)

| Component | GPIO Pin | Logic / Notes |
| --- | --- | --- |
| **PIR Sensor** | `1` | **Input.** HC-SR501 module. Rising Edge (High) triggers wake-up/capture. |
| **Mode Switch** | `14` | **Input.** Hardware Debounced. High = Maintenance; Low (GND) = Field. |
| **Flash LED** | `21` | **Output.** Pulled LOW at idle. High during capture sequence. |
| **SD Card** | `38, 39, 40` | **SDMMC 1-bit.** CMD(38), CLK(39), D0(40). |
| **Camera** | *CSI* | **UXGA Resolution.** OV2640 Configured for `CAMERA_GRAB_LATEST`. |

---

## 🚀 Production Optimization: Disabling Logs

To achieve the fastest "Trigger-to-Photo" response, you must silence the **2nd Stage Bootloader**. This reduces wake-up latency by approximately **300ms**.

### Why it is necessary:

* **Latency:** Default logs create "dead time" before `app_main()` starts.
* **Power:** Minimizes CPU "on-time" during PIR triggers, extending battery life.
* **Reliability:** Prevents the CPU from stalling while waiting for slow Serial (UART) buffer flushes.

### How to Toggle:

1. Run `idf.py menuconfig`.
2. Navigate to **Bootloader config** → **Bootloader log verbosity**.
3. **Production:** Set to `No output`.
4. **Development:** Set to `Info` to troubleshoot boot sequences.

---

## 🌐 Web Portal Features (Maintenance Mode)

When the device is in **Maintenance Mode** (Switch High), it hosts an interactive web portal:

* **☀️ Solar Telemetry:** Real-time display of calculated Sunrise, Sunset, and Day/Night status for the Modbury area.
* **📸 Remote Capture:** Trigger manual snapshots with real-time "Capturing..." status feedback.
* **📂 Smart File List:** View all `.jpg` files sorted by timestamp (**newest first**) with individual **View**, **Save**, and **Delete** actions.
* **💾 Bulk Operations:** * **Save All:** Batch-downloads the entire gallery. Includes a **2.5-second safety gap** and Blob URL management to ensure ESP32 SD-bus stability during high-volume transfers.
* **Delete All:** Permanently clears the SD card for fresh deployment.


* **📊 Storage Monitor:** Visual progress bar showing total capacity, used space, and percentage remaining on the 32GB SD card.

---

## ☀️ Solar-Aware Smart Profiles

The camera intelligently adjusts exposure settings without an external photo-resistor:

* **Solar Math:** Upon trigger, the system calculates the sun's position for **Modbury, SA** () based on the day of the year and the **Equation of Time** (orbital "wobble") correction.
* **True Dark Buffer:** "Night Mode" logic includes a **15-minute buffer** (active 15 mins after sunset and ending 15 mins before sunrise).
* **Dynamic Profiles:**
* **Day:** High-speed Auto Exposure (AEC) and Auto Gain (AGC). Flash DISABLED.
* **Night:** Fixed long-exposure (AEC 250), high gain (AGC 20), and 6500K Color Balancing. Flash ENABLED.



---

## 🕒 RTC Time Persistence & Sync

The TrailCam maintains an accurate clock without a constant internet connection via **RTC Memory Restoration**.

### How it Works:

1. **Anchoring:** In Maintenance Mode, the system syncs with `pool.ntp.org` via SNTP. Once the year 2026 is confirmed, the time is "anchored" into `RTC_DATA_ATTR` memory.
2. **Persistence:** Variables marked with `RTC_DATA_ATTR` survive Light Sleep. The ESP32-S3's internal low-power timer tracks elapsed time while the main CPU is off.
3. **Restoration:** Upon PIR wakeup, the system calls `settimeofday(&sleep_anchor_time, NULL)`. The ESP-IDF framework automatically adds the sleep duration to this anchor, providing an instant, accurate timestamp for the photo filename.

---

## 🔄 System Logic & State Machine

### 1. Boot & Clock Restoration

Upon waking from Light Sleep, the system checks the `time_is_set` flag. If valid, it restores the wall-clock time from **RTC Memory**. This ensures photos are timestamped correctly (e.g., `2026_02_21_121500.jpg`) immediately upon boot.

### 2. Field Mode (The Sleep Cycle)

* **Trigger:** PIR goes High → ESP32 wakes.
* **Validation:** Enforces a **10-second rate limit** via `last_success_time_sec` to prevent rapid-fire false triggers.
* **Sequence:** Flash ON → Capture → Flash OFF.
* **Cooldown:** The system polls the PIR sensor every 50ms and refuses to sleep until the PIR signal returns to LOW.

### 3. Maintenance Mode

* Active WiFi and SNTP synchronization.
* Updates the RTC anchor once a year >= 2026 is detected.
* Enables the Web Portal for SD card management and system debugging.

---

## 💾 Persistent RTC Memory Map

Stored in **RTC Slow Memory**, surviving Light Sleep:

* `sleep_anchor_time`: The system clock state saved immediately before entering sleep.
* `time_is_set`: Boolean flag ensuring valid time restoration.
* `last_success_time_sec`: The epoch timestamp of the last successful photo for rate limiting.

---
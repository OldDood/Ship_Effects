
# 🌲 TrailCam S3 v1.0

A high-performance, ESP32-S3 based trail camera featuring Light Sleep optimization, RTC time persistence, and a web-based maintenance portal.

---
## 📝 Deployment Steps

1. **Physical Setup:** - Ensure the **Mode Switch** is set to **Maintenance** (High).
   - Insert a FAT32-formatted microSD card (up to 32GB recommended for 1-bit mode stability).
2. **Initial Sync:** - Power on the device. Connect to the local WiFi.
   - Wait for the Web Portal to indicate "Time Synced" (Status: Year 2026).
3. **Deployment:** - Flip the switch to ON **Field Mode** (Low).
   - The device will perform a "Final Teardown" of WiFi resources and enter Light Sleep.
   - Mount the camera. It is now armed for PIR triggers.
3. **Web Interface:** - Flip the switch to OFF **Maintenance Mode** (High). This disconnects internal 4 AAA Battery pack.
    - Power device via either USB interface.
    - This enables the WIFI and Web Interface on the local house network. (10.0.0.45)    
    - Open Web Browser at adress above.
    - Web Browser will provide all functionallity to manage photos stored on the devices SSD card .

## 🛠 Hardware Pinout (Freenove ESP32-S3)
## 🛠 Hardware Power Supply (Buck Convertor 4.5 to 20VDC in. 5VDC Out (Adjustable))
## 🛠 Hardware Schematic (Trail Camera.kicad_sch)

| Component | GPIO Pin | Logic / Notes |
| :--- | :--- | :--- |
| **PIR Sensor** | `1` | **Input.** Pir sensor module hc-sr501. Rising Edge (High) triggers wake-up/capture. |
| **Mode Switch** | `14` | **Input.** No Pull-up or Pull-down. Hardware Debounced. High = Maintenance; Low (GND) = Field. |
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

* **📸 Remote Capture:** Trigger manual snapshots with real-time "Capturing..." status feedback.
* **📂 Smart File List:** View all `.jpg` files sorted by timestamp (**newest first**) with individual **View**, **Save**, and **Delete** actions.
* **💾 Bulk Operations:** * **Save All:** Batch-downloads the entire gallery with a **3-second safety gap** between files to ensure SD card stability.
    * **Delete All:** Permanently clears the SD card for fresh deployment.
* **📊 Storage Monitor:** Visual progress bar showing total capacity, used space, and percentage remaining.

---

## 🕒 RTC Time Persistence & Sync

The TrailCam is designed to maintain an accurate clock without needing a constant internet connection. This is achieved via **RTC Memory Restoration**.

### How it Works:
1.  **Anchoring:** In Maintenance Mode, the system syncs with `pool.ntp.org` via SNTP. Once the year 2026 is confirmed, the time is "anchored" into `RTC_DATA_ATTR` memory.
2.  **Persistence:** Variables marked with `RTC_DATA_ATTR` survive Light Sleep. The ESP32-S3's internal low-power timer tracks elapsed time while the main CPU is off.
3.  **Restoration:** Upon PIR wakeup, the system calls `settimeofday(&sleep_anchor_time, NULL)`. The ESP-IDF framework automatically adds the sleep duration to this anchor, providing an instant, accurate timestamp for the photo filename.

### Key Benefits:
* **Timestamped Photos:** Files are named correctly (e.g., `2026_02_05_081004.jpg`) immediately upon boot.
* **Intelligent Rate Limiting:** The camera uses the restored RTC clock to ensure it respects the **10-second cooldown** between captures, even though the device was powered down in between.

## 🔄 System Logic & State Machine

### 1. Boot & Clock Restoration
Upon waking from Light Sleep, the system checks the `time_is_set` flag. If valid, it restores the wall-clock time from **RTC Memory** using the `sleep_anchor_time`. This ensures photos are timestamped correctly even without a fresh WiFi sync.

### 2. Field Mode (The Sleep Cycle)
* **Trigger:** PIR goes High → ESP32 wakes. If the pin is already High on boot, the system "latches" the trigger manually.
* **Validation:** Enforces a **10-second rate limit** via `last_success_time_sec` to prevent rapid-fire false triggers.
* **Sequence:** Flash ON (100ms) → Capture → Flash OFF (500ms).
* **Cooldown:** The system polls the PIR sensor every 50ms and refuses to sleep until the PIR signal returns to LOW, preventing immediate re-wakeups.

### 3. Maintenance Mode
* Keeps WiFi and SNTP active.
* Once a year >= 2026 is detected via SNTP, the RTC anchor is updated.
* The physical mode switch is debounced in both directions by an RC network connected to the switch contacts.

---

## 💾 Persistent RTC Memory Map
These variables are stored in **RTC Slow Memory**, surviving Light Sleep but resetting on hard power loss:

* `sleep_anchor_time`: The system clock state saved immediately before entering sleep.
* `time_is_set`: Boolean flag ensuring we don't restore "junk" time data.
* `last_success_time_sec`: The epoch timestamp of the last successful photo for rate limiting.

---

```
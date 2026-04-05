That is a very important distinction to maintain in your documentation. Even though we stripped the **Solar Telemetry** from the *visual* web interface to keep the UI clean and fast, the **Solar Engine** and **NTP synchronization** are still the "brains" of the ESP32's background logic.

The NTP is critical because it tells the ship exactly what time it is, and the Solar Engine uses that time to decide if the "Night Mode" lighting should be active.

I have updated the **Project Architecture** and **Execution Flow** sections to clarify that while the web portal is now a "Management & Storage" tool, the NTP and Solar logic remain the core background services.

---

### 🚢 ShipEffects S3 v1.0

A high-performance automation and telemetry engine for a ship model display, running on the **ESP32-S3**. This project focuses on **RTC time persistence**, **Solar-aware logic**, and a permanent **Web Management Portal**.

---

## 🛠 Project Architecture: "Always-On" Management
* **Network Priority:** Initializes NVS and WiFi immediately to establish the Web Portal.
* **SNTP Integration:** Background service synchronizes with `pool.ntp.org` to anchor the system clock to **2026**; essential for driving the autonomous solar schedule.
* **Storage:** High-stability SDMMC 1-bit interface for audio and effect assets (**32GB Capacity supported**).
* **Asynchronous Web Handshake:** Uses `XMLHttpRequest` (XHR) for non-blocking file transfers with real-time telemetry.

---

## 🛠 Hardware Pinout (Freenove ESP32-S3)

| Component | GPIO Pin | Configuration / Notes |
| :--- | :--- | :--- |
| **SDMMC CLK** | `39` | **Output.** 1-bit mode. Drive strength: `GPIO_DRIVE_CAP_3`. |
| **SDMMC CMD** | `38` | **Output.** 1-bit mode. Drive strength: `GPIO_DRIVE_CAP_3`. |
| **SDMMC D0** | `40` | **In/Out.** 1-bit mode. Drive strength: `GPIO_DRIVE_CAP_3`. |
| **I/O Overhead** | *Multiple* | Camera pins (CSI) are now **floating/available** to free up resources for I2S audio and WLED expansion. |

---

## ☀️ Background Solar Engine (Logic Only)
The system calculates the sun's position for **Adelaide, SA** ($34.9285^\circ$ S) to drive time-of-day lighting effects.
* **Autonomous Operation:** Logic remains active in the background for effect triggering, though removed from the Web UI for performance.
* **Equation of Time:** Adjusts for Earth's orbital "wobble" using a 365-day solar declination model.
* **Night Detection:** Uses a **15-minute "True Dark" buffer** (15 mins post-sunset to 15 mins pre-sunrise) to transition lighting states.

---

## 🌐 Web Management Portal (v1.1)
A sanitized, text-only management interface optimized for reliability and large file handling.
* **Real-Time Progress Tracking:** Integrated XHR progress bar providing 0–100% visual feedback during MP3 uploads.
* **Transfer Control:** Added an **Abort/Cancel** interrupt to safely terminate active uploads and clear the ESP32 network buffer.
* **File Operations:**
    * **Play:** Opens a dedicated browser audio stream in a new tab for effect preview.
    * **Delete:** Individual file removal with URL-safe encoding.
    * **Wipe:** Bulk SD formatting/cleanup utility.
* **Storage Telemetry:** Dynamically calculates and displays remaining capacity (MB) vs. total card volume.

---

## 🕒 RTC Time Persistence
Maintains chronological integrity across Light Sleep or software resets without requiring a new WiFi handshake.
* **`sleep_anchor_time`:** Stores the `timeval` struct in **RTC Slow Memory**.
* **Auto-Restoration:** On boot, if `time_is_set` is true, the system calls `settimeofday` to instantly restore the wall-clock from the RTC anchor.

---

## 🚀 Execution Flow (`app_main`)
1.  **NVS Init:** Prepares flash for WiFi credentials.
2.  **Service Start:** Mounts **SD Card** $\rightarrow$ Starts **WiFi** $\rightarrow$ Configures **SNTP** $\rightarrow$ Launches **Web Portal**.
3.  **Clock Check:** Determines if the boot is a "Cold Boot" or a "Wakeup." If a wakeup, it restores time from RTC memory.
4.  **Solar Update:** Enters the main loop while background tasks handle the HTTP server and SD card I/O.

---

## 💾 Persistent RTC Memory Map
* `last_success_time_sec`: Tracking for effect rate-limiting.
* `sleep_anchor_time`: The master clock reference.
* `time_is_set`: Guard flag for valid time restoration.

---
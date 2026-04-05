# 🚢 ShipEffects S3 v1.0

A high-performance automation and telemetry engine for a ship model display, running on the **ESP32-S3**. This project focuses on **RTC time persistence**, **Solar-aware logic**, and a permanent **Web Management Portal**.

---

## 🛠 Project Architecture: "Always-On" Management
* **Network Priority:** Initializes NVS and WiFi immediately to establish the Web Portal.
* **SNTP Integration:** Synchronizes with `pool.ntp.org` to anchor the system clock to **2026**.
* **Storage:** High-stability SDMMC 1-bit interface for audio and effect assets.

---

## 🛠 Hardware Pinout (Freenove ESP32-S3)

| Component | GPIO Pin | Configuration / Notes |
| :--- | :--- | :--- |
| **SDMMC CLK** | `39` | **Output.** 1-bit mode. Drive strength: `GPIO_DRIVE_CAP_3`. |
| **SDMMC CMD** | `38` | **Output.** 1-bit mode. Drive strength: `GPIO_DRIVE_CAP_3`. |
| **SDMMC D0** | `40` | **In/Out.** 1-bit mode. Drive strength: `GPIO_DRIVE_CAP_3`. |
| **I/O Overhead** | *Multiple* | Camera pins (CSI) are now **floating/available** for future I2S/WLED expansion. |

---

## ☀️ Solar Telemetry Engine
The system calculates the sun's position for **Adelaide, SA** ($34.9285^\circ$ S) to drive time-of-day lighting effects.
* **Equation of Time:** Adjusts for Earth's orbital "wobble" using a 365-day solar declination model.
* **Night Detection:** Uses a **15-minute "True Dark" buffer** (15 mins post-sunset to 15 mins pre-sunrise).
* **Web Integration:** Calculated `sunrise_mins` and `sunset_mins` are exposed via C-linkage for display on the Web Portal.

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
4.  **Solar Update:** Enters the main loop while the background tasks handle solar math and web requests.

---

## 💾 Persistent RTC Memory Map
* `last_success_time_sec`: Tracking for effect rate-limiting.
* `sleep_anchor_time`: The master clock reference.
* `time_is_set`: Guard flag for valid time restoration.

---
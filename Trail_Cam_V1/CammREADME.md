## 🌲 Trail Cam v1.0 | Project Documentation

### 🛠 Hardware Pinout (ESP32-S3)

| Component | GPIO Pin | Logic / Notes |
| --- | --- | --- |
| **PIR Sensor** | `PIR_SENSOR_GPIO` | Input (Internal/External Pull-down). Triggers on High. |
| **Mode Switch** | `FIELD_MODE_SW_GPIO` | High = Maintenance (WiFi On), Low = Field (Deep Sleep). |
| **SD Card** | *Standard SPI/MMC* | Stores timestamped JPG images. |
| **Camera** | *CSI Interface* | Configured for high-speed capture on wakeup. |

---

### 🔄 System Logic & State Machine

1. **Cold Boot:** User flips power on. System syncs time via SNTP if in Maintenance Mode.
2. **Field Mode (The Loop):**
* **Trigger:** PIR goes High → ESP32 wakes up.
* **Validation:** Restores time from RTC memory. Checks if >10s since last photo.
* **Action:** Captures photo, updates "Last Success" timestamp.
* **Cleanup:** Waits for PIR pulse to end (scoped @ 1.12s) to prevent re-triggering.
* **Sleep:** Saves current time to RTC "Anchor" and enters Deep Sleep.


3. **Maintenance Mode:**
* WiFi and Web Portal stay active.
* System continuously updates the RTC time anchor so it's ready for the next Field deployment.



---

### 💾 Persistent RTC Memory Map

These variables survive **Deep Sleep** but are lost on **Hard Reset/Power Loss**:

* `sleep_anchor_time`: The "Running Clock" saved right before sleep.
* `time_is_set`: Boolean flag indicating if we have ever synced with the real world.
* `last_success_time_sec`: The epoch timestamp of the last successful photo (for rate limiting).

---

### ⚠️ Current Known Issues ("The Bug List")

* **PIR Latch:** PIR sometimes requires a specific settling window; currently handled by a 100ms arming latch check.
* **Mode Transition:** Requires a PIR trigger or reboot to "wake up" the logic if switch is flipped while the device is already in Deep Sleep.

---

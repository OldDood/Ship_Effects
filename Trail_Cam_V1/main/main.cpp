/* TrailCam Main Application
See CammREADME.md for project overview and details.
*/

#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "web_portal.hpp"
// Camera & SD Includes
#include "esp_camera.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include "esp_sleep.h"
#include "esp_pm.h"
#include "driver/gpio.h"
#include <cmath> // For the solar math

// This header includes your Menuconfig WiFi settings
#include "sdkconfig.h"
#include "main.h"

static const char *TAG = "TrailCam";

// WiFi Macros from your menuconfig
#define WIFI_SSID CONFIG_WIFI_SSID
#define WIFI_PASS CONFIG_WIFI_PASS

static EventGroupHandle_t s_wifi_event_group;

// Freenove S3 Pin Mapping
#define XCLK_GPIO_NUM 15
#define SIOD_GPIO_NUM 4
#define SIOC_GPIO_NUM 5
#define Y9_GPIO_NUM 16
#define Y8_GPIO_NUM 17
#define Y7_GPIO_NUM 18
#define Y6_GPIO_NUM 12
#define Y5_GPIO_NUM 10
#define Y4_GPIO_NUM 8
#define Y3_GPIO_NUM 9
#define Y2_GPIO_NUM 11
#define VSYNC_GPIO_NUM 6
#define HREF_GPIO_NUM 7
#define PCLK_GPIO_NUM 13

#define PIR_SENSOR_GPIO 1 // The PIR motion sensor is connected to GPIO1 (ADC1_CH0 / Touch 1) on the S3. This is a clean GPIO that can be used for interrupts and wakeup without ADC or touch conflicts.
#define MODE_SW_GPIO 14   // The physical on/off switch for Field/Maintenance mode. Active LOW (pulls to GND when in Field mode).
#define FLASH_LED_GPIO 21 // The flash LED is connected to GPIO21 on the S3. This pin will be set HIGH to turn on the flash during photo capture, and LOW otherwise.

static esp_netif_t *sta_netif = NULL;                // Global handle for the default Wi-Fi station network interface
static esp_event_handler_instance_t instance_any_id; // Global handle for the Wi-Fi event handler (any ID)
static esp_event_handler_instance_t instance_got_ip; // Global handle for the IP event handler (got IP)

// Motion Detection Flag
static volatile bool motion_detected = false;
// Maintenance mode is true after debouncing the switch
// The on/off switch on the case is off
static volatile bool maintenance_mode_enabled = false;

// For switch debouncing
static uint32_t last_switch_check_time = 0;

// These flags prevent repeated logging of sleep entries, which can be noisy in the logs
static bool motion_sleep_logged = false;
static bool idle_sleep_logged = false;

static bool camera_initialized = false;

// This variable survives Light Sleep
// It tracks the last time a photo was taken
RTC_DATA_ATTR uint64_t last_success_time_sec = 0;

// This survives Light Sleep!
// It is used to track elapsed time across sleeps
RTC_DATA_ATTR struct timeval sleep_anchor_time;
// Flag to indicate if time has been set
RTC_DATA_ATTR bool time_is_set = false;

// SD Card Handle
static sdmmc_card_t *card = NULL;

/**
 * @brief Consolidated GPIO Initialization for ESP32-S3-WROOM-1
 * * Mapping Logic:
 * - GPIO 21 (Output): Flash LED. (Hardware verified via Arduino).
 * - GPIO 1  (Input):  PIR Sensor. (Configured for Rising Edge Interrupt).
 * On S3, this is a clean GPIO (ADC1_CH0 / Touch 1).
 * - GPIO 14 (Input):  Field Mode Switch. (Internal Pull-up enabled).
 * Expects switch to pull to GND.
 */
void init_project_gpios()
{
    gpio_config_t io_conf = {};

    // --- 1. FLASH LED (Output) ---
    io_conf.pin_bit_mask = (1ULL << FLASH_LED_GPIO);
    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    io_conf.pull_down_en = GPIO_PULLDOWN_ENABLE;
    gpio_config(&io_conf);
    gpio_set_level((gpio_num_t)FLASH_LED_GPIO, 0); // Start OFF

    // --- 2. PIR SENSOR (Input + Interrupt + Wakeup Ready) ---
    io_conf.pin_bit_mask = (1ULL << PIR_SENSOR_GPIO);
    io_conf.mode = GPIO_MODE_INPUT;
    // Set for Rising Edge Trigger (PIR High = Motion) for when the chip is awake
    io_conf.intr_type = GPIO_INTR_POSEDGE;
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    io_conf.pull_down_en = GPIO_PULLDOWN_ENABLE; // Ensure LOW state when idle
    gpio_config(&io_conf);

    /* --- S3 Light SLEEP WAKEUP CONFIG --- */

    // Configure the specific PIR pin to wake the chip on a HIGH level.
    // Light sleep wakeup on S3 requires LEVEL triggers, not EDGE.
    // gpio_wakeup_enable((gpio_num_t)PIR_SENSOR_GPIO, GPIO_INTR_HIGH_LEVEL);
    // This is the S3-standard way to wake from a HIGH level on a specific pin

    esp_sleep_enable_ext1_wakeup(1ULL << PIR_SENSOR_GPIO, ESP_EXT1_WAKEUP_ANY_HIGH);

    NewFunction(io_conf);
}

void NewFunction(gpio_config_t &io_conf)
{
    // --- 3. FIELD MODE SWITCH (Input) ---
    io_conf.pin_bit_mask = (1ULL << MODE_SW_GPIO);
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE; // Active LOW switch
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    gpio_config(&io_conf);

    ESP_LOGI("GPIO", "S3-WROOM-1 GPIOs Initialized: Flash(%d), PIR(%d), Switch(%d)",
             FLASH_LED_GPIO, PIR_SENSOR_GPIO, MODE_SW_GPIO);
}
// 1. WiFi Handler
static void event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
    {
        esp_wifi_connect();
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
    {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI("WIFI", "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        // The Bit-setting line is gone.
        // Our while(1) loop will see the time jump to 2026 shortly after this.
    }
}

// 2. WiFi Init
void init_wifi()
{
    s_wifi_event_group = xEventGroupCreate();
    esp_netif_init();
    esp_event_loop_create_default();
    sta_netif = esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);

    // Register event handlers and capture the instance handles for later unregistration
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT,
        ESP_EVENT_ANY_ID,
        &event_handler,
        NULL,
        &instance_any_id // <--- Capture the handle here
        ));

    // Register the IP event handler for when we get an IP address, and capture the instance handle
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT,
        IP_EVENT_STA_GOT_IP,
        &event_handler,
        NULL,
        &instance_got_ip // <--- Capture the handle here
        ));

    wifi_config_t wifi_config = {};
    strncpy((char *)wifi_config.sta.ssid, WIFI_SSID, sizeof(wifi_config.sta.ssid));
    strncpy((char *)wifi_config.sta.password, WIFI_PASS, sizeof(wifi_config.sta.password));

    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    esp_wifi_start();
    ESP_LOGI(TAG, "WiFi Connected!");
    esp_wifi_set_ps(WIFI_PS_NONE);
    ESP_LOGI(TAG, "WiFi Power Save Disabled (for stable SNTP)");
}

// 3. SNTP / Time Init
void time_sync_notification_cb(struct timeval *tv)
{
    ESP_LOGI("SNTP", "Notification: Time has been synchronized!");
}

// SNTP Init Function
// This function sets up SNTP with explicit server configuration
// and timezone settings.
// SNTP is used to keep the system time accurate for timestamping photos.
void init_sntp()
{
    if (esp_sntp_enabled())
    {
        esp_sntp_stop(); // Restart it to be sure
    }

    ESP_LOGI("SNTP", "Starting SNTP service with explicit config...");

    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);

    // Use the IP address of the pool as well as the name to bypass DNS issues
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_setservername(1, "203.0.178.191"); // au.pool.ntp.org (Oceania)

    // Force an immediate sync notification
    sntp_set_sync_mode(SNTP_SYNC_MODE_IMMED);
    // Set your timezone to Australia Central Standard Time (ACST)
    setenv("TZ", "ACST-9:30ACDT,M10.1.0,M4.1.0", 1);
    tzset();
    // As soon as the time is updated from the internet, set a system notificaion.
    sntp_set_time_sync_notification_cb(time_sync_notification_cb);
    esp_sntp_init();
}

// 4. Camera Init
esp_err_t init_camera()
{
    // Check if already initialized
    if (camera_initialized)
    {
        ESP_LOGI("CAM", "Camera already initialized. Skipping...");
        return ESP_OK;
    }

    camera_config_t config;
    config.ledc_channel = LEDC_CHANNEL_0;
    config.ledc_timer = LEDC_TIMER_0;
    config.pin_d0 = Y2_GPIO_NUM;
    config.pin_d1 = Y3_GPIO_NUM;
    config.pin_d2 = Y4_GPIO_NUM;
    config.pin_d3 = Y5_GPIO_NUM;
    config.pin_d4 = Y6_GPIO_NUM;
    config.pin_d5 = Y7_GPIO_NUM;
    config.pin_d6 = Y8_GPIO_NUM;
    config.pin_d7 = Y9_GPIO_NUM;
    config.pin_xclk = XCLK_GPIO_NUM;
    config.pin_pclk = PCLK_GPIO_NUM;
    config.pin_vsync = VSYNC_GPIO_NUM;
    config.pin_href = HREF_GPIO_NUM;
    config.pin_sccb_sda = SIOD_GPIO_NUM;
    config.pin_sccb_scl = SIOC_GPIO_NUM;
    config.pin_pwdn = -1;
    config.pin_reset = -1;
    config.xclk_freq_hz = 10000000;
    config.frame_size = FRAMESIZE_UXGA;
    config.pixel_format = PIXFORMAT_JPEG;
    config.fb_location = CAMERA_FB_IN_PSRAM;
    config.jpeg_quality = 20; // 0-63 lower means higher quality (and larger file size)
    config.fb_count = 2;      // Allocating 2 frame buffers for smoother capture
    config.grab_mode = CAMERA_GRAB_LATEST;

    esp_err_t err = esp_camera_init(&config);
    if (err == ESP_OK)
    {
        camera_initialized = true;
        ESP_LOGI("CAM", "Camera initialized successfully.");
    }
    else
    {
        ESP_LOGE("CAM", "Camera init failed with error 0x%x", err);
    }

    return err;
}

// 5. SD Card Init
esp_err_t init_sd_card()
{
    // If the card pointer is already set, the card is already initialized.
    if (card != NULL)
    {
        ESP_LOGI(TAG, "SD card already initialized. Skipping...");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Initializing SD card");

    // Define the mount point name
#define MOUNT_POINT "/sdcard"

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024,
        .disk_status_check_enable = false,
        .use_one_fat = false};

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = 5000; // Lower to 5MHz for stability

    // Freenove S3 uses specific pins for SDMMC
    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.width = 1;
    slot_config.clk = (gpio_num_t)39;
    slot_config.cmd = (gpio_num_t)38;
    slot_config.d0 = (gpio_num_t)40;
    slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    // Boost the signal strength to the SD card pins
    gpio_set_drive_capability((gpio_num_t)39, GPIO_DRIVE_CAP_3); // CLK
    gpio_set_drive_capability((gpio_num_t)38, GPIO_DRIVE_CAP_3); // CMD
    gpio_set_drive_capability((gpio_num_t)40, GPIO_DRIVE_CAP_3); // D0 (Data)

    esp_err_t ret = esp_vfs_fat_sdmmc_mount(MOUNT_POINT, &host, &slot_config, &mount_config, &card);

    if (ret != ESP_OK)
    {
        if (ret == ESP_FAIL)
        {
            ESP_LOGE(TAG, "Failed to mount filesystem. Card might need formatting.");
        }
        else
        {
            ESP_LOGE(TAG, "Failed to initialize the card (%s). Check pins/card.", esp_err_to_name(ret));
        }
        return ret;
    }

    ESP_LOGI(TAG, "SD Card mounted successfully!");
    return ESP_OK;
}

// 6. SD Card Uninitialise
esp_err_t ret = ESP_OK; // Declare and initialize outside the if block

esp_err_t un_init_sd_card()
{
    if (card != NULL)
    {
        ESP_LOGI(TAG, "Unmounting SD card...");
        ret = esp_vfs_fat_sdcard_unmount(MOUNT_POINT, card);

        if (ret == ESP_OK)
        {
            card = NULL; // Only clear the handle if unmount actually worked
            ESP_LOGI(TAG, "Unmounted the SD card successfully.");
        }
        else
        {
            ESP_LOGE(TAG, "Failed to unmount the SD card.");
        }
    }
    return ret;
}

bool is_solar_night(struct tm *ti)
{
    // 1. Get Day of Year (0-365)
    int day = ti->tm_yday;

    // 2. Solar Approximation for Adelaide/Modbury
    // Sunrise/Sunset shift for -34.8 Latitude
    // These constants approximate the South Australian seasonal shift
    float sunrise_mins = 400 + 70 * cos((day + 10) * 0.0172);
    float sunset_mins = 1070 - 100 * cos((day + 10) * 0.0172);

    // 3. Apply your 15-minute "True Dark" buffers
    int night_start = (int)sunset_mins + 15;
    int night_end = (int)sunrise_mins - 15;

    // 4. Current time in minutes from midnight
    int current_mins = (ti->tm_hour * 60) + ti->tm_min;

    // 5. Check if we are in the night zone (handles wrap-around at midnight)
    if (current_mins >= night_start || current_mins <= night_end)
    {
        return true;
    }
    return false; // If current time is between night_end and night_start, it's not solar night
}

// 1. New dedicated function to handle the sensor personality
void apply_smart_profile(sensor_t *s, uint8_t ambient_lux)
{
    // Ensure the image is always oriented correctly
    s->set_vflip(s, 1);
    s->set_hmirror(s, 1);

    if (ambient_lux < 80)
    {
        ESP_LOGI(TAG, "Night Detected. Activating 6500K Flash Profile.");
        s->set_exposure_ctrl(s, 0);
        s->set_aec_value(s, 250); // Your calibrated AEC 0-1200, lower means longer exposure
        s->set_gain_ctrl(s, 0);
        s->set_agc_gain(s, 20);      // Your calibrated AGC 0-30, higher means more gain
        s->set_wb_mode(s, 1);        // Sunny/6500K white balance mode, which is a good match for our flash color
        s->set_contrast(s, 1);       // Slight contrast boost for better detail in shadows
        s->set_special_effect(s, 2); // Apply a blue-ish tint to compensate for the warm flash. This is a bit of a hack, but it can help balance the color in very dark conditions. Adjust or remove as needed based on your testing.
    }
    else
    {
        ESP_LOGI(TAG, "Daylight Detected. Using Auto Profile.");

        s->set_exposure_ctrl(s, 1);
        s->set_gain_ctrl(s, 1);
        s->set_whitebal(s, 1);
        s->set_wb_mode(s, 0);
        s->set_special_effect(s, 0); // No special effect in daylight
        s->set_contrast(s, 0);       // Default contrast for daylight
    }
}

/// 6. Capture Function - Takes Photo and Saves to SD
extern "C" esp_err_t take_photo()
{
    ESP_LOGI("TrailCam", "Initiating capture sequence...");

    // 1. GET TIME ONCE (Consolidated at the top to fix redeclaration errors)
    struct tm timeinfo;
    time_t now = time(NULL);
    localtime_r(&now, &timeinfo);

    // 2. SOLAR LOGIC & SENSOR SETUP
    bool night_mode = is_solar_night(&timeinfo);

    ESP_LOGI(TAG, "Solar Analysis: %02d:%02d | %s Mode",
             timeinfo.tm_hour,
             timeinfo.tm_min,
             night_mode ? "NIGHT" : "DAY");

    // Spoof the lux to trigger your existing if/else logic in apply_smart_profile
    uint8_t spoofed_lux = night_mode ? 20 : 200;

    sensor_t *s = esp_camera_sensor_get();
    apply_smart_profile(s, spoofed_lux);

    // --- FLASH CONTROL: ON ---
    gpio_set_level((gpio_num_t)FLASH_LED_GPIO, 1);

    // 3. --- WARM-UP / FLUSH ---
    for (int i = 0; i < 4; i++)
    {
        camera_fb_t *fb_temp = esp_camera_fb_get();
        if (fb_temp)
            esp_camera_fb_return(fb_temp);
    }

    // Capture the actual frame
    camera_fb_t *fb = esp_camera_fb_get();

    // --- FLASH CONTROL: OFF ---
    // Note: Removed the vTaskDelay here to ensure we don't hold the flash on during SD writes
    gpio_set_level((gpio_num_t)FLASH_LED_GPIO, 0);

    // 4. ERROR CHECKING
    if (!fb)
    {
        ESP_LOGE("TrailCam", "Camera capture failed!");
        return ESP_FAIL;
    }

    // 5. FILENAME GENERATION (Using the timeinfo we declared at the top)
    char filename[100];
    snprintf(filename, sizeof(filename), "/sdcard/%04d_%02d_%02d_%02d%02d%02d.jpg",
             timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
             timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);

    ESP_LOGI("TrailCam", "Opening file: %s", filename);
    FILE *file = fopen(filename, "wb");

    if (file == NULL)
    {
        ESP_LOGE("TrailCam", "Failed! errno: %d (%s)", errno, strerror(errno));
        esp_camera_fb_return(fb);
        return ESP_FAIL;
    }

    // 6. SD WRITE
    size_t written = fwrite(fb->buf, 1, fb->len, file);
    fclose(file);

    if (written == fb->len)
    {
        ESP_LOGI("TrailCam", "SUCCESS! Saved %d bytes", (int)fb->len);
        esp_camera_fb_return(fb);
        return ESP_OK;
    }
    else
    {
        ESP_LOGE("TrailCam", "Write Error!");
        esp_camera_fb_return(fb);
        return ESP_FAIL;
    }
}

extern "C"
{
    // 1. Declaration: Remove IRAM_ATTR here
    void pir_sensor_isr_handler(void *arg);

    // 2. Definition: Keep IRAM_ATTR here
    void IRAM_ATTR pir_sensor_isr_handler(void *arg)
    {
        motion_detected = true;
        gpio_set_intr_type(GPIO_NUM_1, GPIO_INTR_DISABLE);
    }
}

// 5. START MODE-SPECIFIC SERVICES
void maintenance_mode_init()
{
    ESP_LOGI(TAG, "MAINTENANCE MODE: Starting Network...");
    init_wifi();
    init_sntp();
    start_web_portal();
}

void field_mode_init()
{
    stop_web_portal();

    if (esp_sntp_enabled())
    {
        esp_sntp_stop();
    }

    // 1. Stop the Wi-Fi Driver
    esp_wifi_stop();

    // 2. Unregister handlers (to prevent memory leaks/dangling pointers)
    esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, instance_any_id);
    esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, instance_got_ip);

    // 3. De-init Wi-Fi
    esp_wifi_deinit();

    // 4. Destroy the Netif (This is the missing link!)
    if (sta_netif)
    {
        esp_netif_destroy_default_wifi(sta_netif);
        sta_netif = NULL; // Critical: set to NULL so your guard works
    }

    ESP_LOGI(TAG, "FIELD MODE: Resources cleared and Wi-Fi fully unloaded.");
}

// 7. MAIN APPLICATION ENTRY POINT
// MAIN ENTRY
extern "C" void app_main()
{

    // 1. HARDWARE & TIME RESTORATION
    init_project_gpios();

    esp_sleep_wakeup_cause_t wakeup_reason = esp_sleep_get_wakeup_cause();

    // Use the variable to check the cause
    if (time_is_set && (wakeup_reason == ESP_SLEEP_WAKEUP_GPIO || wakeup_reason == ESP_SLEEP_WAKEUP_TIMER))
    {
        settimeofday(&sleep_anchor_time, NULL);
        ESP_LOGI("TIME", "Woke up (cause %d). Clock restored from RTC Memory.", wakeup_reason);
    }
    else if (!time_is_set)
    {
        ESP_LOGI("TIME", "Cold boot or time not yet synced.");
    }

    // 2. BOOT-TIME MODE CHECK
    vTaskDelay(pdMS_TO_TICKS(50));
    maintenance_mode_enabled = (gpio_get_level((gpio_num_t)MODE_SW_GPIO) == true);

    // 3. INIT PERIPHERALS

    init_camera();

    // 4. INTERRUPT SETUP
    nvs_flash_init();
    esp_err_t err = gpio_install_isr_service(0);
    if (err == ESP_OK || err == ESP_ERR_INVALID_STATE)
    {
        gpio_isr_handler_add((gpio_num_t)PIR_SENSOR_GPIO, pir_sensor_isr_handler, (void *)PIR_SENSOR_GPIO);
    }

    if (maintenance_mode_enabled)
    {
        ESP_LOGI(TAG, "Switch is in Maintenance position at boot.");
        maintenance_mode_init();
    }
    else
    {
        ESP_LOGI(TAG, "Switch is in Field position at boot.");
        field_mode_init();
    }

    // 6. PIR STABILIZATION & LATCHING
    ESP_LOGI(TAG, "Arming PIR sensor...");

    // Arm the interrupt FIRST
    gpio_set_intr_type((gpio_num_t)PIR_SENSOR_GPIO, GPIO_INTR_POSEDGE);

    // Small delay to let the electrical state settle
    vTaskDelay(pdMS_TO_TICKS(100));

    // THE FIX: If the pin is ALREADY high, it won't fire a POSEDGE interrupt.
    // We must manually trip the flag for the motion that woke us up!
    if (gpio_get_level((gpio_num_t)PIR_SENSOR_GPIO) == 1)
    {
        ESP_LOGI(TAG, "PIR is already HIGH. Latching initial trigger.");
        motion_detected = true;
    }
    else
    {
        motion_detected = false;
    }

    // --- MAIN LOOP ---
    while (1)
    {
        struct timeval now;
        gettimeofday(&now, NULL);
        struct tm timeinfo;
        localtime_r(&now.tv_sec, &timeinfo);

        // Update anchor if SNTP just synced the clock (Year 2026 check)
        if (maintenance_mode_enabled && timeinfo.tm_year >= 126)
        {
            if (!time_is_set)
            {
                time_is_set = true;
                ESP_LOGI("TIME", "SNTP Sync Success.");
            }
            sleep_anchor_time = now;
        }

        // --- STEP 1: CONTINUOUS MODE MONITORING ---
        if (esp_log_timestamp() - last_switch_check_time > 50)
        {
            last_switch_check_time = esp_log_timestamp();
            bool field_mode_sw_pos = (gpio_get_level((gpio_num_t)MODE_SW_GPIO) == true);

            if (field_mode_sw_pos) // Physically in Maintenance position
            {
                // Transition to Maintenance
                if (!maintenance_mode_enabled)
                {
                    maintenance_mode_enabled = true;
                    ESP_LOGI(TAG, "Entering Maintenance Mode...");
                    maintenance_mode_init();
                }
            }
            else // Physically in Field position
            {
                // Transition to Field (Only when counter hits 0)
                if (maintenance_mode_enabled)
                {
                    maintenance_mode_enabled = false;
                    ESP_LOGI(TAG, "Entering Field Mode...");
                    field_mode_init();
                }
            }
        }

        // MOTION & PHOTO LOGIC
        if (motion_detected)
        {
            // 10-second Rate Limiter
            if (last_success_time_sec != 0 && (now.tv_sec - last_success_time_sec) < 10)
            {
                ESP_LOGW(TAG, "Rate limiting: Only %lds since last photo.", (long)(now.tv_sec - last_success_time_sec));
            }
            else
            {
                init_sd_card();
                init_camera();
                take_photo();
                gettimeofday(&now, NULL);
                last_success_time_sec = now.tv_sec;
            }

            // Wait for Pulse to end
            while (gpio_get_level((gpio_num_t)PIR_SENSOR_GPIO) == 1)
            {
                vTaskDelay(pdMS_TO_TICKS(50));
            }

            motion_detected = false;
            gpio_set_intr_type((gpio_num_t)PIR_SENSOR_GPIO, GPIO_INTR_POSEDGE);

            if (!maintenance_mode_enabled)
            {
                if (!motion_sleep_logged)
                {
                    gettimeofday(&sleep_anchor_time, NULL);
                    sleep_anchor_time.tv_sec += 2;
                    // Wait for PIR to go back to 0 so we don't immediately wake back up
                    ESP_LOGI(TAG, "Waiting for PIR to clear...");
                    while (gpio_get_level((gpio_num_t)PIR_SENSOR_GPIO) == 1)
                    {
                        vTaskDelay(pdMS_TO_TICKS(100)); // Check every 100ms
                    }

                    ESP_LOGI(TAG, "PIR clear. Entering Sleep.");
                    ESP_LOGI(TAG, "Entering Light Sleep...");
                    motion_sleep_logged = true; // Prevents re-logging
                    un_init_sd_card();          // Unmount SD card before sleeping to prevent corruption
                    esp_camera_deinit();
                    camera_initialized = false; // Reset camera init flag so it will re-init after sleep
                    vTaskDelay(pdMS_TO_TICKS(5000));
                    esp_light_sleep_start();
                    motion_sleep_logged = false; // Reset the flag after waking up, so we can log again on the next motion event
                }
            }
            else
            {
                vTaskDelay(pdMS_TO_TICKS(5000));
            }
        }

        // Idle Timeout
        if (!maintenance_mode_enabled && !idle_sleep_logged && esp_log_timestamp() > 20000)
        {
            /*             // Wait for PIR to go back to 0 so we don't immediately wake back up
                        ESP_LOGI(TAG, "Waiting for PIR to clear...");
                        while (gpio_get_level((gpio_num_t)PIR_SENSOR_GPIO) == 1)
                        {
                            vTaskDelay(pdMS_TO_TICKS(100)); // Check every 100ms
                        }

                        ESP_LOGI(TAG, "PIR clear. Entering Sleep.");
                        un_init_sd_card(); // Unmount SD card before sleeping to prevent corruption
                        esp_camera_deinit();
                        camera_initialized = false; // Reset camera init flag so it will re-init after sleep
                        ESP_LOGI(TAG, "Idle Timeout. Sleeping...");
                        idle_sleep_logged = true; // Prevents re-logging
                        vTaskDelay(pdMS_TO_TICKS(5000)); // Short delay to ensure logs are flushed before sleeping
                        esp_light_sleep_start(); */
        }

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
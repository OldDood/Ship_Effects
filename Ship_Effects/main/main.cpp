/* Ship Effects Main Application
See ShipREADME.md for project overview and details.
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
//#include "esp_camera.h"
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

static const char *TAG = "ShipEffects";

// WiFi Macros from your menuconfig
#define WIFI_SSID CONFIG_WIFI_SSID
#define WIFI_PASS CONFIG_WIFI_PASS

static EventGroupHandle_t s_wifi_event_group;


static int sunrise_mins = 0; 
static int sunset_mins = 0;
extern "C" int get_sunrise_mins() { return sunrise_mins; }
extern "C" int get_sunset_mins() { return sunset_mins; }


static esp_netif_t *sta_netif = NULL;                // Global handle for the default Wi-Fi station network interface
static esp_event_handler_instance_t instance_any_id; // Global handle for the Wi-Fi event handler (any ID)
static esp_event_handler_instance_t instance_got_ip; // Global handle for the IP event handler (got IP)


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


// Adelaide, South Australia Coordinates
const float ADL_LAT = -34.9285;
const float ADL_LON = 138.6007;

extern "C" bool is_solar_night(struct tm *ti) {
    // 1. Determine UTC Offset for Adelaide
    // tm_isdst > 0 means Daylight Savings is active
    float tz_offset = (ti->tm_isdst > 0) ? 10.5 : 9.5;

   
    // --- START SOLAR MATH ---
    float rad = M_PI / 180.0;
    int day = ti->tm_yday;

    // Solar Declination: Position of sun relative to equator
    float declination = 23.45 * sin(rad * (360.0 / 365.0) * (day - 81));
    
    // Equation of Time: Adjusts for Earth's orbital "wobble"
    float b = rad * (360.0 / 364.0) * (day - 81);
    float eot = 9.87 * sin(2 * b) - 7.53 * cos(b) - 1.5 * sin(b);
    
    // Hour Angle: The distance from noon to sunrise/sunset
    float cos_h = (cos(rad * 90.833) - sin(rad * ADL_LAT) * sin(rad * declination)) / 
                  (cos(rad * ADL_LAT) * cos(rad * declination));
    
    // Calculate sunrise/sunset in UTC minutes, then shift to Adelaide local time
    float h = acos(cos_h) / rad; 
    float solar_noon_utc = 720.0 - (4.0 * ADL_LON) - eot;
    
    sunrise_mins = (int)(solar_noon_utc - (h * 4.0) + (tz_offset * 60.0));
    sunset_mins = (int)(solar_noon_utc + (h * 4.0) + (tz_offset * 60.0));
    // --- END SOLAR MATH ---

    // Logging the results for Adelaide
    ESP_LOGI(TAG, "Today is Day %d of the year", ti->tm_yday);
    ESP_LOGI(TAG, "Calculated Sunrise: %02d:%02d", sunrise_mins / 60, sunrise_mins % 60);
    ESP_LOGI(TAG, "Calculated Sunset:  %02d:%02d", sunset_mins / 60, sunset_mins % 60);

    // 3. Apply your 15-minute "True Dark" buffers
    int night_start = sunset_mins + 15;
    int night_end = sunrise_mins - 15;

    // 4. Current time in minutes from midnight
    int current_mins = (ti->tm_hour * 60) + ti->tm_min;

    // 5. Check if we are in the night zone (handles wrap-around at midnight)
    if (current_mins >= night_start || current_mins <= night_end) {
        return true;
    }
    return false;
}



// 7. MAIN APPLICATION ENTRY POINT
// MAIN ENTRY
extern "C" void app_main()
{
    // 1. MUST INITIALIZE NVS FIRST FOR WIFI TO WORK
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // 2. NOW START SERVICES
    ESP_LOGI(TAG, "Initialised NVS Flash. Starting WiFi, SNTP, SD Card, and Web Portal...");
    init_sd_card();
    init_wifi();
    init_sntp();
    start_web_portal();

    // 3. HARDWARE & TIME RESTORATION
    esp_sleep_wakeup_cause_t wakeup_reason = esp_sleep_get_wakeup_cause();

    if (time_is_set && (wakeup_reason == ESP_SLEEP_WAKEUP_GPIO || wakeup_reason == ESP_SLEEP_WAKEUP_TIMER))
    {
        settimeofday(&sleep_anchor_time, NULL);
        ESP_LOGI("TIME", "Woke up. Clock restored from RTC Memory.");
    }
    else if (!time_is_set)
    {
        ESP_LOGI("TIME", "Cold boot or time not yet synced.");
    }

    // --- MAIN LOOP ---
    while (1)
    {
        // ... your loop logic ...
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
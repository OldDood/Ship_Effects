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

// This header includes your Menuconfig WiFi settings
#include "sdkconfig.h"

static const char *TAG = "TrailCam";

// WiFi Macros from your menuconfig
#define WIFI_SSID      CONFIG_WIFI_SSID
#define WIFI_PASS      CONFIG_WIFI_PASS
#define WIFI_CONNECTED_BIT BIT0
static EventGroupHandle_t s_wifi_event_group;

// Freenove S3 Pin Mapping
#define XCLK_GPIO_NUM     15
#define SIOD_GPIO_NUM      4
#define SIOC_GPIO_NUM      5
#define Y9_GPIO_NUM       16
#define Y8_GPIO_NUM       17
#define Y7_GPIO_NUM       18
#define Y6_GPIO_NUM       12
#define Y5_GPIO_NUM       10
#define Y4_GPIO_NUM        8
#define Y3_GPIO_NUM        9
#define Y2_GPIO_NUM       11
#define VSYNC_GPIO_NUM     6
#define HREF_GPIO_NUM      7
#define PCLK_GPIO_NUM     13

#define PIR_SENSOR_GPIO 1
#define FIELD_MODE_SW_GPIO 14
#define FLASH_LED_GPIO 21

// Motion Detection Flag
static volatile bool motion_detected = false;

/**
 * @brief Consolidated GPIO Initialization for ESP32-S3-WROOM-1
 * * Mapping Logic:
 * - GPIO 21 (Output): Flash LED. (Hardware verified via Arduino).
 * - GPIO 1  (Input):  PIR Sensor. (Configured for Rising Edge Interrupt). 
 * On S3, this is a clean GPIO (ADC1_CH0 / Touch 1).
 * - GPIO 14 (Input):  Field Mode Switch. (Internal Pull-up enabled).
 * Expects switch to pull to GND.
 */
void init_project_gpios() {
    gpio_config_t io_conf = {};

    // --- 1. FLASH LED (Output) ---
    io_conf.pin_bit_mask = (1ULL << FLASH_LED_GPIO);
    io_conf.mode         = GPIO_MODE_OUTPUT;
    io_conf.intr_type    = GPIO_INTR_DISABLE;
    io_conf.pull_up_en   = GPIO_PULLUP_DISABLE;
    io_conf.pull_down_en = GPIO_PULLDOWN_ENABLE;
    gpio_config(&io_conf);
    gpio_set_level((gpio_num_t)FLASH_LED_GPIO, 0); // Start OFF

    // --- 2. PIR SENSOR (Input + Interrupt Ready) ---
    io_conf.pin_bit_mask = (1ULL << PIR_SENSOR_GPIO);
    io_conf.mode         = GPIO_MODE_INPUT;
    // Set for Rising Edge Trigger (PIR High = Motion)
   io_conf.intr_type    = GPIO_INTR_POSEDGE; 
    io_conf.pull_up_en   = GPIO_PULLUP_DISABLE;
    io_conf.pull_down_en = GPIO_PULLDOWN_ENABLE; // Ensure LOW state when idle
    gpio_config(&io_conf);

    // --- 3. FIELD MODE SWITCH (Input) ---
    io_conf.pin_bit_mask = (1ULL << FIELD_MODE_SW_GPIO);
    io_conf.mode         = GPIO_MODE_INPUT;
    io_conf.intr_type    = GPIO_INTR_DISABLE;
    io_conf.pull_up_en   = GPIO_PULLUP_ENABLE;   // Active LOW switch
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    gpio_config(&io_conf);

    ESP_LOGI("GPIO", "S3-WROOM-1 GPIOs Initialized: Flash(%d), PIR(%d), Switch(%d)", 
             FLASH_LED_GPIO, PIR_SENSOR_GPIO, FIELD_MODE_SW_GPIO);
}


// 1. WiFi Handler
static void event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

// 2. WiFi Init
void init_wifi() {
    s_wifi_event_group = xEventGroupCreate();
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL, NULL);

    wifi_config_t wifi_config = {};
    strncpy((char*)wifi_config.sta.ssid, WIFI_SSID, sizeof(wifi_config.sta.ssid));
    strncpy((char*)wifi_config.sta.password, WIFI_PASS, sizeof(wifi_config.sta.password));
    
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    esp_wifi_start();
    xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE, portMAX_DELAY);
    ESP_LOGI(TAG, "WiFi Connected!");
    esp_wifi_set_ps(WIFI_PS_NONE); 
    ESP_LOGI(TAG, "WiFi Power Save Disabled (for stable SNTP)");
}

// 3. SNTP / Time Init
void time_sync_notification_cb(struct timeval *tv) {
    ESP_LOGI("SNTP", "Notification: Time has been synchronized!");
}
void init_sntp() {
    if (esp_sntp_enabled()) {
        esp_sntp_stop(); // Restart it to be sure
    }

    ESP_LOGI("SNTP", "Starting SNTP service with explicit config...");
    
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    
    // Use the IP address of the pool as well as the name to bypass DNS issues
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_setservername(1, "203.0.178.191"); // au.pool.ntp.org (Oceania)

    // Force an immediate sync notification
    sntp_set_sync_mode(SNTP_SYNC_MODE_IMMED);
    
    setenv("TZ", "ACST-9:30ACDT,M10.1.0,M4.1.0", 1);
    tzset();

    sntp_set_time_sync_notification_cb(time_sync_notification_cb);
    esp_sntp_init();
}

// 4. Camera Init
esp_err_t init_camera() {
    camera_config_t config;
    config.ledc_channel = LEDC_CHANNEL_0;
    config.ledc_timer = LEDC_TIMER_0;
    config.pin_d0 = Y2_GPIO_NUM; config.pin_d1 = Y3_GPIO_NUM;
    config.pin_d2 = Y4_GPIO_NUM; config.pin_d3 = Y5_GPIO_NUM;
    config.pin_d4 = Y6_GPIO_NUM; config.pin_d5 = Y7_GPIO_NUM;
    config.pin_d6 = Y8_GPIO_NUM; config.pin_d7 = Y9_GPIO_NUM;
    config.pin_xclk = XCLK_GPIO_NUM; config.pin_pclk = PCLK_GPIO_NUM;
    config.pin_vsync = VSYNC_GPIO_NUM; config.pin_href = HREF_GPIO_NUM;
    config.pin_sccb_sda = SIOD_GPIO_NUM; config.pin_sccb_scl = SIOC_GPIO_NUM;
    config.pin_pwdn = -1; config.pin_reset = -1;
    config.xclk_freq_hz = 10000000;
    config.frame_size = FRAMESIZE_UXGA;
    config.pixel_format = PIXFORMAT_JPEG;
    config.fb_location = CAMERA_FB_IN_PSRAM;
    config.jpeg_quality = 12;
    config.fb_count = 2;
    config.grab_mode = CAMERA_GRAB_LATEST;

    return esp_camera_init(&config);
}

// 5. SD Card Init
esp_err_t init_sd_card() {
    ESP_LOGI(TAG, "Initializing SD card");

    // Define the mount point name
#define MOUNT_POINT "/sdcard"

esp_vfs_fat_sdmmc_mount_config_t mount_config = {
    .format_if_mount_failed = false,
    .max_files = 5,
    .allocation_unit_size = 16 * 1024,
    .disk_status_check_enable = false,
    .use_one_fat = false 
};

    sdmmc_card_t *card;
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

    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGE(TAG, "Failed to mount filesystem. Card might need formatting.");
        } else {
            ESP_LOGE(TAG, "Failed to initialize the card (%s). Check pins/card.", esp_err_to_name(ret));
        }
        return ret;
    }

    ESP_LOGI(TAG, "SD Card mounted successfully!");
    sdmmc_card_print_info(stdout, card);
    return ESP_OK;
}

// 6. Capture Function - Takes Photo and Saves to SD
extern "C" esp_err_t take_photo() {
    ESP_LOGI("TrailCam", "Initiating capture sequence...");

    // --- FLASH CONTROL: ON ---
    gpio_set_level((gpio_num_t)FLASH_LED_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(100)); // Stabilize light/exposure

    camera_fb_t * fb = esp_camera_fb_get();

    // --- FLASH CONTROL: OFF ---motion
    vTaskDelay(pdMS_TO_TICKS(500)); // Stabilize light/exposure
    gpio_set_level((gpio_num_t)FLASH_LED_GPIO, 0);
  
    // Check if capture was successful
    if (!fb) {
        ESP_LOGE("TrailCam", "Camera capture failed!");
        return ESP_FAIL;
    }

    char filename[100];
    memset(filename, 0, sizeof(filename));

    struct tm timeinfo;
    time_t now = time(NULL);
    localtime_r(&now, &timeinfo);

    // Keep your exact Format: YYYY_MM_DD_HHMMSS.jpg
    snprintf(filename, sizeof(filename), "/sdcard/%04d_%02d_%02d_%02d%02d%02d.jpg", 
             timeinfo.tm_year+1900,  timeinfo.tm_mon + 1, timeinfo.tm_mday,
             timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);            
  
    ESP_LOGI("TrailCam", "Opening file: %s", filename);
    FILE* file = fopen(filename, "wb");
    
    if (file == NULL) {
        ESP_LOGE("TrailCam", "Failed! errno: %d (%s)", errno, strerror(errno));
        esp_camera_fb_return(fb);
        return ESP_FAIL;
    }

    size_t written = fwrite(fb->buf, 1, fb->len, file);
    fclose(file);

    if (written == fb->len) {
        ESP_LOGI("TrailCam", "SUCCESS! Saved %d bytes", (int)fb->len);
        esp_camera_fb_return(fb);
        return ESP_OK;
    } else {
        ESP_LOGE("TrailCam", "Write Error!");
        esp_camera_fb_return(fb);
        return ESP_FAIL;
    }
}


extern "C" {
    void IRAM_ATTR pir_sensor_isr_handler(void* arg);
}

// Then the definition:
void IRAM_ATTR pir_sensor_isr_handler(void* arg) 
{
    motion_detected = true;
    gpio_set_intr_type(GPIO_NUM_1, GPIO_INTR_DISABLE);    
}

// 7. MAIN APPLICATION ENTRY POINT
// MAIN ENTRY
    extern "C" void app_main() {
    // 1. Initialize NVS (Required for WiFi)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Initialize GPIOs
    init_project_gpios();
    
    // 2. Hardware Setup
    init_sd_card();
    vTaskDelay(pdMS_TO_TICKS(500)); // Wait 500ms
    init_camera();
    vTaskDelay(pdMS_TO_TICKS(500)); // Wait 500ms
    

    // 3. Network Setup
    init_wifi(); 
    vTaskDelay(pdMS_TO_TICKS(1000));

    // Call your function instead of the raw system call!
init_sntp(); 

// 4. Now wait for the sync to actually happen
ESP_LOGI(TAG, "Waiting for SNTP sync...");
int retry = 0;
// We wait until the status is NOT RESET anymore
while (sntp_get_sync_status() == SNTP_SYNC_STATUS_RESET && ++retry < 100) {
    vTaskDelay(pdMS_TO_TICKS(500)); // Real 500ms delay
}

time_t now;
struct tm timeinfo;
time(&now);
localtime_r(&now, &timeinfo);

if (timeinfo.tm_year > (1970 - 1900)) { // If year is later than 1970
    ESP_LOGI(TAG, "Time synchronized successfully! Current year: %d", timeinfo.tm_year + 1900);
} else {
    ESP_LOGW(TAG, "SNTP sync timed out. Using default system time.");
}

// 5. Start the Web Portal
    start_web_portal();

    // 6. Maintenance Loop vs. Capture Loop
    // For now, since we are testing Maintenance Mode, we stop here.
    ESP_LOGI(TAG, "System is now in Maintenance Mode. Photo loop suspended.");

esp_err_t err = gpio_install_isr_service(0);

if (err == ESP_OK) {
    gpio_isr_handler_add((gpio_num_t)PIR_SENSOR_GPIO, pir_sensor_isr_handler, (void*)PIR_SENSOR_GPIO);
    printf("ISR service installed successfully.\n");
} else if (err == ESP_ERR_INVALID_STATE) {
    printf("ISR service was already installed. Skipping.\n");
} else {
    printf("Failed to install ISR service: %s\n", esp_err_to_name(err));
}

// 2. Hook the handler to the specific GPIO pin
gpio_isr_handler_add((gpio_num_t)PIR_SENSOR_GPIO, pir_sensor_isr_handler, (void*)PIR_SENSOR_GPIO);
    
    while (1) {
       if (motion_detected == 1) {
            ESP_LOGI("TrailCam", "Motion detected! Calling take_photo...");
            
            // Now it is safe to call take_photo because we are in a Task, not an ISR
            if (take_photo() == ESP_OK) {
                ESP_LOGI("TrailCam", "Capture complete.");
            }

            // Move your 10-second "cool down" here
            vTaskDelay(pdMS_TO_TICKS(10000)); 

            // Reset and re-enable
            motion_detected = false;
            gpio_set_intr_type(GPIO_NUM_1, GPIO_INTR_POSEDGE);
        }

        // Small delay to let the CPU breathe
        vTaskDelay(pdMS_TO_TICKS(10));
    }

}
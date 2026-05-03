/* Ship Effects Main Application
See ShipREADME.md for project overview and details.
*/
#include "driver/uart.h"

// Define the UART port if it isn't defined by the board config
#ifndef UART_NUM_1
#define UART_NUM_1 (uart_port_t)1
#endif

#include <errno.h>
#include <string.h>
#include <stdio.h>
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
// #include "esp_camera.h"
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

#include "driver/i2s_std.h" // For I2S audio output to the MAX98357A
#define MINIMP3_IMPLEMENTATION
#include "minimp3.h"

#include "freertos/semphr.h" // Required for the Mutex

#include <sys/stat.h>

#include <dirent.h>

#define MAX_MARKERS 16 // We are limiting to 16 for now as per your plan

// --- Begin defining the data structures for the Audio Markers and Timeline
// 1. Define what a single Marker looks like
typedef struct
{
    uint32_t trigger_ms; // The time in milliseconds (calculated from the CSV)
    uint8_t marker_id;   // The ID (1 to 16)
    bool triggered;      // A "latch" to ensure it only fires once per song
} audio_marker_t;

// 2. Define the Container (The Timeline)
typedef struct
{
    audio_marker_t markers[MAX_MARKERS];
    uint16_t count;          // How many markers were actually loaded
    SemaphoreHandle_t mutex; // The "Lock" for thread safety
} marker_timeline_t;

// 3. Create the Global Instance
// This is the "Shared Notepad" both tasks will use.
marker_timeline_t ship_timeline;

// --- I2S Audio Bus (The Digital Stream) ---
#define I2S_BCLK_IO (GPIO_NUM_7) // Bit Clock: Synchronizes each individual bit of audio data.
#define I2S_LRC_IO (GPIO_NUM_15) // Left/Right Clock (Word Select): Tells the amp which "word" is Left vs Right channel.
#define I2S_DIN_IO (GPIO_NUM_6)  // Data In (to Amp): The actual digital audio signal stream being sent to the MAX98357A.

// --- Amplifier Control (Hardware Logic) ---
#define SPEAKER_SD_IO (GPIO_NUM_4)   // Shutdown Control: Drive HIGH to enable the amp; drive LOW for silent/low-power mode.
#define SPEAKER_GAIN_IO (GPIO_NUM_5) // Gain Adjust: Controls volume floor. Set LOW (GND) for 12dB; leave floating or HIGH for higher gain.

static const char *TAG = "ShipEffects";

volatile bool AUDIO_ENABLED= false; // Global flag to control audio playback. Set to true when music should play, false to stop immediately.
uint8_t current_volume = 50; // Default volume percentage (0-100)

 // External function located in main.cpp
extern "C" void set_master_volume(int vol_percent);
// Global volume (0.0 to 1.0). 
// Using a float for smooth multiplication in the audio loop.
static float master_volume = 0.5f; 

// WiFi Macros from your menuconfig
#define WIFI_SSID CONFIG_WIFI_SSID
#define WIFI_PASS CONFIG_WIFI_PASS

static EventGroupHandle_t s_wifi_event_group;
static EventGroupHandle_t s_system_event_group;
#define WIFI_CONNECTED_BIT BIT0
#define SNTP_SYNCED_BIT BIT1


void load_timeline_from_csv(const char *file_path); // Forward declaration of the function to load the timeline from a CSV file
void set_wled_bus_value(uint8_t value);             // Forward declaration of the function to set the WLED bus value based on the marker ID
void trigger_autoplay_from_sd();                    // Forward declaration of the function to trigger autoplay from SD card on boot
/**
 * @brief Prototype for the WLED Serial Bridge
 * Sends a JSON preset command based on the timeline marker_id.
 */
void send_wled_command(uint8_t marker_id); // Forward declaration of the function to send a command to WLED based on the marker ID
void init_wled_serial(void); // Forward declaration of the function to initialize the UART for WLED communication

// Volatile tells the compiler that this variable can change 
// unexpectedly (since it's shared between two different CPU cores).
volatile bool is_audio_playing = false;// This flag will be set to true when music starts playing, and false when it stops. Both the audio task and the main loop can read this variable to know if music is currently playing or not.

// This queue will hold the filename of the project we want to play
static QueueHandle_t playback_queue = NULL;

// In a header file included by both main.cpp and web_portal.cpp
typedef struct
{
    char filename[64];
    bool sync_enabled;
} playback_cmd_t;

// This allows web_portal to call the function located in main.cpp
extern "C" void trigger_project_play(playback_cmd_t *cmd)
{
    if (playback_queue != NULL)
    {
        // We send a copy of the struct into the queue
        // This is safe even if the web server handler finishes and 'cmd' goes out of scope
        xQueueSend(playback_queue, cmd, pdMS_TO_TICKS(10));
    }
    else
    {
        ESP_LOGE("MAIN", "Playback queue not initialized!");
    }
}

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
// I2S Audio Channel Handle
static i2s_chan_handle_t tx_handle = NULL;

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
        // SIGNAL: WiFi is ready!
        xEventGroupSetBits(s_system_event_group, WIFI_CONNECTED_BIT);
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
    // SIGNAL: Time is ready!
    xEventGroupSetBits(s_system_event_group, SNTP_SYNCED_BIT);
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
        .max_files = 10,
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


void trigger_autoplay_from_sd()
{
    FILE *f = fopen("/sdcard/autoplay.txt", "r");
    if (f == NULL)
    {
        ESP_LOGI("BOOT", "No autoplay.txt found. Waiting for web command.");
        return;
    }

    playback_cmd_t cmd;
    memset(&cmd, 0, sizeof(playback_cmd_t));

    if (fgets(cmd.filename, sizeof(cmd.filename), f) != NULL)
    {
        // 1. Remove ANY trailing whitespace or control characters (\r, \n, spaces)
        int len = strlen(cmd.filename);
        while (len > 0 && (cmd.filename[len - 1] == '\n' ||
                           cmd.filename[len - 1] == '\r' ||
                           cmd.filename[len - 1] == ' '))
        {
            cmd.filename[--len] = '\0';
        }

        // 2. Safety check: Ensure we didn't end up with an empty string
        if (strlen(cmd.filename) > 0)
        {
            cmd.sync_enabled = true;
            ESP_LOGI("BOOT", "Auto-playing: [%s]", cmd.filename);
            is_audio_playing = true; // Lock the system immediately!
            trigger_project_play(&cmd);
        }
    }
    fclose(f);
}

// Adelaide, South Australia Coordinates
const float ADL_LAT = -34.9285;
const float ADL_LON = 138.6007;

extern "C" bool is_solar_night(struct tm *ti)
{
    // If it's 1970, we have no idea where the sun is.
    if (ti->tm_year < (2025 - 1900))
    {
        ESP_LOGW(TAG, "Time not synced. Defaulting to 'Night Mode' for safety.");
        return true;
    }

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
    if (current_mins >= night_start || current_mins <= night_end)
    {
        return true;
    }
    return false;
}

// 7. Speaker Hardware Init
void init_speaker_hardware()
{
    ESP_LOGI(TAG, "Initializing MAX98357A Amplifier Pins...");

    // 1. Ensure Gain is set before waking up
    gpio_reset_pin(SPEAKER_GAIN_IO);
    gpio_set_direction(SPEAKER_GAIN_IO, GPIO_MODE_OUTPUT);
    gpio_set_level(SPEAKER_GAIN_IO, 1);

    // 2. Wake up the Amp (SD Pin)
    gpio_reset_pin(SPEAKER_SD_IO);
    gpio_set_direction(SPEAKER_SD_IO, GPIO_MODE_OUTPUT);

    // Hold it LOW for a split second to settle
    gpio_set_level(SPEAKER_SD_IO, 0);
    vTaskDelay(pdMS_TO_TICKS(100));

    // Drive HIGH to enable
    gpio_set_level(SPEAKER_SD_IO, 1);
    ESP_LOGI(TAG, "Amplifier Pins Configured on Core %d", xPortGetCoreID());
} // <--- Bracket closed correctly here

void init_i2s_driver()
{
    ESP_LOGI(TAG, "Configuring I2S for MAX98357A...");

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &tx_handle, NULL));

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(44100),
        /* --- CHANGE START: Set to STEREO so the clock speed matches the data --- */
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        /* --- CHANGE END --- */
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = I2S_BCLK_IO,
            .ws = I2S_LRC_IO,
            .dout = I2S_DIN_IO,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {.mclk_inv = false, .bclk_inv = false, .ws_inv = false},
        },
    };

    ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_handle, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(tx_handle));

    ESP_LOGI(TAG, "I2S Driver Enabled in STEREO mode for correct timing.");
}

// 7a. Internal Diagnostic Test Function
void test_speaker_sine_repeater()
{
    const int sample_rate = 44100;
    const int num_steps = 4;
    const int num_cycles = 4;
    float frequencies[num_steps] = {300.0f, 600.0f, 1200.0f, 2400.0f}; // Test frequencies in Hz that are easily audible and distinct, covering a range of the spectrum. Adjust as needed.

    const int buffer_size = 512;  // Number of samples per buffer (must be a multiple of the frame size for 16-bit mono)
    int16_t samples[buffer_size]; // Buffer to hold the generated audio samples
    size_t bytes_written;         // To capture how many bytes were actually written to the I2S driver

    // Use a local phase variable we can reset
    float phase = 0.0f;
    const float amplitude = 12000.0f; // Amplitude for 16-bit audio (max is 32767, but we leave headroom to avoid clipping)

    ESP_LOGI(TAG, "Starting v1.4 Soft-Start Sine Repeater...");
    // This loop will run through the specified frequencies, playing each for 2 seconds, and repeat the whole sequence for the number of cycles defined.
    for (int cycle = 1; cycle <= num_cycles; cycle++)
    {
        ESP_LOGI(TAG, "--- Cycle %d of %d ---", cycle, num_cycles);

        // --- THE CLICK FIX ---
        // Force phase to 0 before starting the burst.
        // This ensures the first sample is sin(0) = 0.
        phase = 0.0f;

        i2s_channel_enable(tx_handle);

        for (int f = 0; f < num_steps; f++)
        {
            float freq = frequencies[f];
            uint32_t step_start = esp_log_timestamp();
            // Generate and play the sine wave for this frequency for 2 seconds
            while (esp_log_timestamp() - step_start < 2000)
            {
                for (int i = 0; i < buffer_size; i++)
                {
                    samples[i] = (int16_t)(amplitude * sinf(phase));    // Generate the sine wave sample
                    phase += (2.0f * M_PI * freq) / (float)sample_rate; // Increment the phase for the next sample
                    if (phase >= 2.0f * M_PI)
                        phase -= 2.0f * M_PI; // Wrap the phase to prevent it from growing indefinitely, which can cause precision issues over time.
                }
                i2s_channel_write(tx_handle, samples, sizeof(samples), &bytes_written, portMAX_DELAY);
            }
        }

        i2s_channel_disable(tx_handle); // Disable the I2S output during the silent period to prevent "hiss" and save power.
        // If this isn't the last cycle, wait 15 seconds before starting the next one
        if (cycle < num_cycles)
        {
            ESP_LOGI(TAG, "Waiting 1 seconds...");
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
}
// 7b. WAV Playback Test Function
void play_wav_file(const char *path, int num_loops)
{
    // FORCE Stereo mode even though the file is Mono
    // This ensures the MAX98357A sees the timing it expects
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(44100),
        .slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = { 
            .mclk = I2S_GPIO_UNUSED, 
            .bclk = (gpio_num_t)7, 
            .ws = (gpio_num_t)15, 
            .dout = (gpio_num_t)6, 
            .din = I2S_GPIO_UNUSED,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false }
        }
    };

   
    i2s_channel_reconfig_std_clock(tx_handle, &std_cfg.clk_cfg);
    i2s_channel_reconfig_std_slot(tx_handle, &std_cfg.slot_cfg);

    for (int i = 1; i <= num_loops; i++)
    {
        FILE *f = fopen(path, "rb");
        if (!f) return;

        // Use the REAPER-confirmed data start
        fseek(f, 690, SEEK_SET); 

        const int chunk_size = 1024;
        int16_t *mono_buffer = (int16_t *)malloc(chunk_size);
        // We need a second buffer to hold the "Stereo" version
        int16_t *stereo_buffer = (int16_t *)malloc(chunk_size * 2); 
        
        size_t bytes_read;
        size_t bytes_written;

        i2s_channel_enable(tx_handle);

        while ((bytes_read = fread(mono_buffer, 1, chunk_size, f)) > 0)
        {
            int samples = bytes_read / sizeof(int16_t);
            for (int s = 0; s < samples; s++) {
                // Copy the same sample to both Left and Right channels
                stereo_buffer[s*2] = mono_buffer[s];     // Left
                stereo_buffer[s*2 + 1] = mono_buffer[s]; // Right
            }
            // Write twice the data (because it's now stereo)
            i2s_channel_write(tx_handle, stereo_buffer, bytes_read * 2, &bytes_written, portMAX_DELAY);
        }

        free(mono_buffer);
        free(stereo_buffer);
        fclose(f);
        i2s_channel_disable(tx_handle);

        if (i < num_loops) vTaskDelay(pdMS_TO_TICKS(1000));
    }
}


void update_i2s_sample_rate(int rate)
{
    ESP_LOGI("SHIP", "Updating I2S rate to: %d Hz", rate);

    // 1. You MUST disable the channel before changing the clock config
    i2s_channel_disable(tx_handle);

    // 2. Configure the new clock settings
    i2s_std_clk_config_t clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG((uint32_t)rate);

    // 3. Apply the reconfiguration
    i2s_channel_reconfig_std_clock(tx_handle, &clk_cfg);

    // 4. Re-enable the channel so it can start pumping data again
    i2s_channel_enable(tx_handle);
}

void init_wled_serial() {
    // 1. Configure the UART parameters
    const uart_config_t uart_config = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
        .flags = 0
    };

    // 2. Install the driver (Port 1, RX buffer 2048, TX buffer 0, no event queue)
    esp_err_t err = uart_driver_install(UART_NUM_1, 2048, 0, 0, NULL, 0);
    if (err != ESP_OK) {
        ESP_LOGE("WLED", "Failed to install UART driver!");
        return;
    }

    // 3. Apply the configuration
    uart_param_config(UART_NUM_1, &uart_config);

    // 4. Set the pins (TX: GPIO 17, RX: GPIO 18)
    // You can change 17/18 to whatever pins you have free on the S3
    uart_set_pin(UART_NUM_1, 17, 18, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    
    ESP_LOGI("WLED", "Serial Bridge Initialized on UART1 (TX:17, RX:18)");
}

void send_wled_command(uint8_t marker_id)
{
    // Safety check: 0 is usually 'off' or 'idle' in WLED, 
    // so we only send if the ID is within our expected range.
    if (marker_id < 1 || marker_id > 16) {
        ESP_LOGW("WLED", "Invalid marker_id: %d (Skipping)", marker_id);
        return;
    }

    char json_cmd[32];
    // Format: {"ps": 1} through {"ps": 16}
    // We include \n because WLED's serial parser uses it as a frame delimiter.
    int len = snprintf(json_cmd, sizeof(json_cmd), "{\"ps\":%d}\n", marker_id);

    // Write to UART1 (Assuming UART1 is your WLED bridge)
    uart_write_bytes(UART_NUM_1, json_cmd, len);
    
    ESP_LOGI("WLED", "Marker %d -> WLED JSON sent: %s", marker_id, json_cmd);
}

void play_mp3_file(const char *path)
{
    // --- ADD THE DIAGNOSTIC BLOCK HERE ---
    DIR *d = opendir("/sdcard");
    if (d)
    {
        struct dirent *de;
        ESP_LOGW("FILESYSTEM", "--- SD Card Directory Listing ---");
        while ((de = readdir(d)) != NULL)
        {
            ESP_LOGI("FILESYSTEM", "Found: [%s]", de->d_name);
        }
        closedir(d);
    }
    // -------------------------------------
    ESP_LOGI("DEBUG", "Attempting to open path: [%s]", path);
    FILE *f = fopen(path, "rb");
    if (f == NULL)
    {
        ESP_LOGE(TAG, "Failed to open MP3: %s", path);
        return;
    }

    ESP_LOGI(TAG, "--- Playing: %s ---", path);

    static mp3dec_t mp3d;
    mp3dec_init(&mp3d);
    mp3dec_frame_info_t info;

    const int read_size = 4096;
    uint8_t *input_buf = (uint8_t *)malloc(read_size);
    int16_t *pcm_buf = (int16_t *)malloc(MINIMP3_MAX_SAMPLES_PER_FRAME * sizeof(int16_t) * 2);

    size_t bytes_left = 0;
    int last_rate = 0;

    // --- NEW: Sync Variables ---
    uint64_t total_bytes_sent = 0;

    while (true)
    {
    // Check the switch EVERY frame. If it's flipped to OFF (0), exit immediately!
// Inside your while(true) decoding loop
if (AUDIO_ENABLED == 0) {
    ESP_LOGW("AUDIO", "Switch DISARMED: Killing playback.");
    break; // Stops the music instantly
}
        size_t n = fread(input_buf + bytes_left, 1, read_size - bytes_left, f);
        bytes_left += n;

        if (bytes_left == 0)
            break;

        int samples = mp3dec_decode_frame(&mp3d, input_buf, bytes_left, pcm_buf, &info);

        if (samples > 0)
        {
            if (info.hz != last_rate)
            {
                update_i2s_sample_rate(info.hz);
                last_rate = info.hz;
            }

            // --- NEW: Apply Volume Scaling ---
            // 'samples' is samples per channel. Total samples = samples * info.channels
            int total_samples = samples * info.channels;
            float current_gain = master_volume; // Local copy to prevent changes mid-buffer

            for (int i = 0; i < total_samples; i++) {
                // Multiply sample by gain and clip to 16-bit boundaries
                int32_t val = (int32_t)(pcm_buf[i] * current_gain);
                if (val > 32767) val = 32767;
                if (val < -32768) val = -32768;
                pcm_buf[i] = (int16_t)val;
            }

            size_t bytes_to_write = samples * info.channels * sizeof(int16_t);
            size_t bytes_written;

            // Send PCM data to I2S
            i2s_channel_write(tx_handle, pcm_buf, bytes_to_write, &bytes_written, portMAX_DELAY);

            // --- SYNC ENGINE START ---
            total_bytes_sent += bytes_written;

            // Calculate current playback time in milliseconds
            // Formula: bytes / (samples_per_sec * channels * bytes_per_sample) * 1000
            // For 44100Hz Stereo 16-bit: total_bytes_sent / 176.4
            uint32_t current_ms = (uint32_t)(total_bytes_sent / (info.hz * info.channels * 2.0 / 1000.0));

            // Check if any markers need to fire
            if (xSemaphoreTake(ship_timeline.mutex, 0) == pdTRUE)
            {
                for (int i = 0; i < ship_timeline.count; i++)
                {
                    if (!ship_timeline.markers[i].triggered && current_ms >= ship_timeline.markers[i].trigger_ms)
                    {

                        ship_timeline.markers[i].triggered = true; // Set flag so it only fires once

                        ESP_LOGI("SYNC", ">>> TRIGGER MARKER %d at %ld ms (Track Time: %ld ms)",
                                 ship_timeline.markers[i].marker_id,
                                 ship_timeline.markers[i].trigger_ms,
                                 current_ms);

                        send_wled_command(ship_timeline.markers[i].marker_id);// The new Serial link;
                    }
                }
                xSemaphoreGive(ship_timeline.mutex);
            }
            // --- SYNC ENGINE END ---
        }

        if (info.frame_bytes > 0)
        {
            bytes_left -= info.frame_bytes;
            memmove(input_buf, input_buf + info.frame_bytes, bytes_left);
        }

        vTaskDelay(1);
    }

    free(input_buf);
    free(pcm_buf);
    fclose(f);
    is_audio_playing = false; // OPEN the gate for the next track
    ESP_LOGI(TAG, "Playback Finished.");
}



extern "C" void set_master_volume(int vol_percent) {
    if (vol_percent < 0) vol_percent = 0;
    if (vol_percent > 100) vol_percent = 100;
    
    // Convert 0-100 to 0.0-1.0
    master_volume = vol_percent / 100.0f;
    ESP_LOGI("AUDIO", "Master Volume set to: %d%%", vol_percent);
}


// 1. Update the task to actually DO the work
void audio_playback_task(void *pvParameters)
{
    ESP_LOGI("SHIP", "Audio Task checking for WiFi/Time sync (10s timeout)...");
    // the I2S ISR (Interrupt Service Routine) will be registered on Core 1.
    init_speaker_hardware();
    init_i2s_driver();
    // Wait for bits, but only for 10 seconds
    EventBits_t bits = xEventGroupWaitBits(
        s_system_event_group,
        WIFI_CONNECTED_BIT | SNTP_SYNCED_BIT, // Wait for either WiFi or SNTP to signal ready
        pdFALSE,
        pdTRUE,
        pdMS_TO_TICKS(10000) // Wait for up to 10 seconds (10000 ms)
    );

    // Check which bits were set and log the results
    if ((bits & (WIFI_CONNECTED_BIT | SNTP_SYNCED_BIT)) == (WIFI_CONNECTED_BIT | SNTP_SYNCED_BIT))
    {
        ESP_LOGI("SHIP", "System ready with Network Time."); // Both WiFi and SNTP are ready, so we have accurate time for sunrise/sunset calculations and photo timestamps.
    }
    else
    {
        ESP_LOGW("SHIP", "WiFi/SNTP failed or timed out. Proceeding with local RTC time."); // Proceed anyway, but the time-based features will be inaccurate until time is synced.
    }

    // Now playback starts regardless of whether WiFi was found

    ESP_LOGI("SHIP", "System Ready! Audio Task proceeding on Core %d", xPortGetCoreID());

    ESP_LOGI("SHIP", "Audio Task Started. Determining mode...");
    int core_id = xPortGetCoreID();
    ESP_LOGI("SHIP", "Audio Task successfully pinned to Core %d", core_id);
    int sound_mode = 3; // Default to MP3

// Map your Kconfig settings here inside the task
#if CONFIG_SHIP_SOUND_INTERNAL_DIAGNOSTIC
    sound_mode = 1;
#elif CONFIG_SHIP_SOUND_WAV_MODE
    sound_mode = 2;
#endif

    // Execute the playback
    switch (sound_mode)
    {
    case 1:
        test_speaker_sine_repeater();
        ESP_LOGI("SHIP", "Diagnostic Finished. Cleaning up...");
        vTaskDelete(NULL); // OK to delete: Bench test is done.
        break;

    case 2:
        play_wav_file("/sdcard/ship_horn.wav", 4);
        ESP_LOGI("SHIP", "WAV Test Finished. Cleaning up...");
        vTaskDelete(NULL); // OK to delete: Horn test is done.
        break;

    case 3:
        ESP_LOGI("SHIP", "Entering Persistent Web Mode. Task will NOT be deleted.");
        playback_cmd_t cmd;
        char full_path[128];

        while (1)
        {
            if (xQueueReceive(playback_queue, &cmd, portMAX_DELAY))
            {
                // 1. Construct the path for the MP3
                snprintf(full_path, sizeof(full_path), "/sdcard/%s.mp3", cmd.filename);

                // 2. Construct the path for the CSV (Marker File)
                char csv_path[128];
                snprintf(csv_path, sizeof(csv_path), "/sdcard/%s.csv", cmd.filename);

                // 3. Load the markers into RAM before starting the audio
                ESP_LOGI("AUDIO_TASK", "New track selected. Loading sync markers from: %s", csv_path);
                load_timeline_from_csv(csv_path);

                // 4. Start the playback
                play_mp3_file(full_path);                
            }
        }
        break; // This line is never actually reached in Case 3.
    }

    ESP_LOGI("SHIP", "Audio Task finished playback. Deleting task.");
}
// 8. Function to Load Timeline from CSV
void load_timeline_from_csv(const char *file_path)
{
    FILE *f = fopen(file_path, "r");
    if (f == NULL)
        return;

    // Constants based on your Reaper Project
    const float bpm = 120.0;
    const int beats_per_measure = 4;
    const float ms_per_beat = (60.0 / bpm) * 1000.0; // 500ms at 120BPM
    const float ms_per_tick = ms_per_beat / 960.0;   // Standard Reaper PPQ

    if (xSemaphoreTake(ship_timeline.mutex, pdMS_TO_TICKS(1000)) == pdTRUE)
    {
        char line[128];
        int marker_index = 0;
        ship_timeline.count = 0;

        fgets(line, sizeof(line), f); // Skip Header

        while (fgets(line, sizeof(line), f) && marker_index < MAX_MARKERS)
        {
            int m, b, t; // Measure, Beat, Tick
            int id_num;
            char freq[32];

            // Parsing: M1, 200Hz, 1.1.86
            if (sscanf(line, "M%d,%[^,],%d.%d.%d", &id_num, freq, &m, &b, &t) == 5)
            {

                // MATH:
                // (Measures-1) * beats * ms_per_beat
                // + (Beats-1) * ms_per_beat
                // + (Ticks * ms_per_tick)
                uint32_t total_ms = ((m - 1) * beats_per_measure * ms_per_beat) +
                                    ((b - 1) * ms_per_beat) +
                                    (t * ms_per_tick);

                ship_timeline.markers[marker_index].trigger_ms = total_ms;
                ship_timeline.markers[marker_index].marker_id = (uint8_t)id_num;
                ship_timeline.markers[marker_index].triggered = false;

                ESP_LOGI("CSV", "Marker %d -> Clock Time: %ld ms", id_num, total_ms);
                marker_index++;
            }
        }
        ship_timeline.count = marker_index;
        xSemaphoreGive(ship_timeline.mutex);
        fclose(f);
    }
}
#include "nvs.h"

// Save the volume to flash memory
extern "C" void save_volume_to_nvs(uint8_t vol) {
    nvs_handle_t my_handle;
    // Open the namespace you initialized in app_main
    esp_err_t err = nvs_open("ship_prefs", NVS_READWRITE, &my_handle);
    if (err == ESP_OK) {
        nvs_set_u8(my_handle, "audio_vol", vol);
        nvs_commit(my_handle); 
        nvs_close(my_handle);
        ESP_LOGI("NVS", "Volume %d latched to Flash", vol);
    }
}

// Load the volume from flash memory
uint8_t load_volume_from_nvs() {
    nvs_handle_t my_handle;
    uint8_t vol = 50; // Default fallback value
    esp_err_t err = nvs_open("ship_prefs", NVS_READONLY, &my_handle);
    if (err == ESP_OK) {
        nvs_get_u8(my_handle, "audio_vol", &vol);
        nvs_close(my_handle);
        ESP_LOGI("NVS", "Loaded volume from NVRAM: %d", vol);
    }
    return vol;
}


void save_play_state_to_nvs(bool state) {
    nvs_handle_t my_handle;
    esp_err_t err = nvs_open("storage", NVS_READWRITE, &my_handle);
    if (err == ESP_OK) {
        nvs_set_u8(my_handle, "play_state", state ? 1 : 0);
        nvs_commit(my_handle);
        nvs_close(my_handle);
    }
}

bool load_play_state_from_nvs() {
    nvs_handle_t my_handle;
    uint8_t play_state = 0; // Default to 0 (Stopped) if not found
    
    esp_err_t err = nvs_open("storage", NVS_READONLY, &my_handle);
    if (err == ESP_OK) {
        // Read the value. If the key "play_state" doesn't exist, play_state remains 0.
        nvs_get_u8(my_handle, "play_state", &play_state);
        nvs_close(my_handle);
    }
    
    return (play_state == 1);
}

// 8. MAIN APPLICATION ENTRY POINT
// MAIN ENTRY
extern "C" void app_main()
{

    // 1. MUST INITIALIZE NVS FIRST FOR WIFI TO WORK
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    

    s_system_event_group = xEventGroupCreate(); // Create the system event group to track WiFi and SNTP readiness

    // 2. NOW START SERVICES
    ESP_LOGI(TAG, "Initialised NVS Flash. Starting WiFi, SNTP, SD Card, and Web Portal...");
    init_sd_card();     // SD Card must be initialized before the web portal, which may serve files from it.
    init_wifi();        // WiFi should be started before SNTP to ensure time can be synced.
    init_sntp();        // Start SNTP to sync time for accurate sunrise/sunset calculations and photo timestamps.
    start_web_portal(); // Start the web portal last, after all hardware and time services are up and running.
    init_wled_serial();

   
    current_volume = load_volume_from_nvs();// Load the saved volume level from flash memory
    set_master_volume(current_volume);// Apply the loaded volume to the audio system immediately

    // 2. Load the saved state from your new function
    AUDIO_ENABLED = load_play_state_from_nvs();
    
    ESP_LOGI(TAG, "Ship Audio Engine is: %s", AUDIO_ENABLED ? "ARMED" : "DISARMED");

    // Launch the playback task with 16KB of stack
    playback_queue = xQueueCreate(5, sizeof(playback_cmd_t));
    xTaskCreatePinnedToCore(audio_playback_task, "AudioTask", 32768, NULL, 5, NULL, 1);

    // The MUTEX purpose is to protect the integrity of the timeline data structure when accessed by multiple tasks. In this case, both the web portal task and the audio playback task may need to read from or write to the timeline (e.g., adding new markers, checking for triggers). The mutex ensures that only one task can access the timeline at a time,
    //  Create the Mutex lock for the timeline
    ship_timeline.mutex = xSemaphoreCreateMutex();
    ship_timeline.count = 0; // Start with an empty list

    if (ship_timeline.mutex == NULL)
    {
        // This is a critical system failure (out of memory, etc.)
        ESP_LOGE("INIT", "FATAL: Failed to create timeline mutex!");
    }
    else
    {
        // Mutex is healthy.
        // We NO LONGER call load_timeline_from_csv here,
        // because the Audio Task will do it dynamically.
        ESP_LOGI("INIT", "Timeline Mutex created successfully.");

        // --- THE GOLDEN MOMENT ---
        // Everything above is now initialized.
        // We wait 1 second to let the Audio Task reach its 'while(1)' loop.
        vTaskDelay(pdMS_TO_TICKS(1000));
        if (AUDIO_ENABLED == 1)
        {
            trigger_autoplay_from_sd();
        }
    }

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
    uint8_t loopback_buf[128]; // Buffer for incoming serial data

    // --- MAIN LOOP ---
    while (1)
    {

        // --- 1. THE LOOPBACK CHECK ---
    // Look at UART_NUM_1 to see if we heard our own TX
    int len = uart_read_bytes(UART_NUM_1, loopback_buf, sizeof(loopback_buf) - 1, 20 / portTICK_PERIOD_MS);
    if (len > 0) {
        loopback_buf[len] = '\0'; // Null-terminate
        ESP_LOGI("DEBUG", "Serial Loopback HEARD: %s", (char*)loopback_buf);
    }

    // --- EXECUTION LOGIC ---
    if (AUDIO_ENABLED == 1 && !is_audio_playing) {
        is_audio_playing = true; // LOCK the gate immediately
        trigger_autoplay_from_sd(); 
    }
    vTaskDelay(pdMS_TO_TICKS(1000)); // Check every second
    }
}
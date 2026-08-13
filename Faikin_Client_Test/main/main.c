#include <string.h>
#include <stdbool.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_http_client.h"

#define EXAMPLE_ESP_WIFI_SSID "DunnysShed"
#define EXAMPLE_ESP_WIFI_PASS "DadsShed"
#define MAXIMUM_RETRY 5

#define FAIKIN_HOST "10.0.0.38"
#define FAIKIN_PORT 80

static const char *TAG_NET = "NET_CORE0";
static const char *TAG_ALGO = "ALGO_CORE1";

static EventGroupHandle_t s_wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1
static int s_retry_num = 0;

typedef struct
{
    float room_temp;
    float target_temp;
    char mode[16];
    bool power;
    bool valid;
} faikin_data_t;

static faikin_data_t g_faikin_state = {0};
static SemaphoreHandle_t g_telemetry_mutex = NULL;

void solar_algo_task(void *pvParameters);

// -------------------------------------------------------------------
// WI-FI SETUP
// -------------------------------------------------------------------

static void event_handler(void *arg, esp_event_base_t event_base,
                          int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
    {
        esp_wifi_connect();
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED)
    {
        if (s_retry_num < MAXIMUM_RETRY)
        {
            esp_wifi_connect();
            s_retry_num++;
        }
        else
        {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
    {
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

void wifi_init_sta(void)
{
    s_wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL, NULL));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = EXAMPLE_ESP_WIFI_SSID,
            .password = EXAMPLE_ESP_WIFI_PASS,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, pdFALSE, pdFALSE, portMAX_DELAY);
}

// -------------------------------------------------------------------
// SSE PARSER
// -------------------------------------------------------------------

static void process_sse_field(const char *event_type, const char *event_data)
{
    if (xSemaphoreTake(g_telemetry_mutex, pdMS_TO_TICKS(100)) == pdTRUE)
    {
        if (strcmp(event_type, "room_temperature") == 0)
        {
            g_faikin_state.room_temp = atof(event_data);
            g_faikin_state.valid = true;
        }
        else if (strcmp(event_type, "temperature") == 0)
        {
            g_faikin_state.target_temp = atof(event_data);
        }
        else if (strcmp(event_type, "mode") == 0)
        {
            strncpy(g_faikin_state.mode, event_data, sizeof(g_faikin_state.mode) - 1);
        }
        else if (strcmp(event_type, "power") == 0)
        {
            g_faikin_state.power = (strcmp(event_data, "ON") == 0 || strcmp(event_data, "on") == 0);
        }

        xSemaphoreGive(g_telemetry_mutex);
    }
}

// CORE 0 TASK: Main HTTP EventStream Receiver Loop
void sse_comms_task(void *pvParameters)
{
    char current_event[32] = {0};

    while (1)
    {
        ESP_LOGI(TAG_NET, "Connecting to Faikin SSE stream at %s/wevents...", FAIKIN_HOST);

        esp_http_client_config_t config = {
            .host = FAIKIN_HOST,
            .port = FAIKIN_PORT,
            .path = "/wevents",
            .timeout_ms = 15000,
        };

        esp_http_client_handle_t client = esp_http_client_init(&config);
        if (!client)
        {
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }

        esp_http_client_set_header(client, "Accept", "text/event-stream");

        esp_err_t err = esp_http_client_open(client, 0);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG_NET, "Failed to open connection: %s", esp_err_to_name(err));
            esp_http_client_cleanup(client);
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }

        esp_http_client_fetch_headers(client);

        char line_buffer[128];
        int line_pos = 0;
        char chunk[64];

        // Continuous streaming loop
        while (1)
        {
            int bytes_read = esp_http_client_read(client, chunk, sizeof(chunk));
            if (bytes_read <= 0)
            {
                ESP_LOGW(TAG_NET, "SSE stream disconnected or timed out. Reconnecting...");
                break;
            }

            for (int i = 0; i < bytes_read; i++)
            {
                char c = chunk[i];
                if (c == '\n')
                {
                    line_buffer[line_pos] = '\0';

                    if (strncmp(line_buffer, "event:", 6) == 0)
                    {
                        // Extract event type
                        const char *val = line_buffer + 6;
                        while (*val == ' ') val++; // Skip spaces
                        strncpy(current_event, val, sizeof(current_event) - 1);
                    }
                    else if (strncmp(line_buffer, "data:", 5) == 0)
                    {
                        // Extract event data payload
                        const char *val = line_buffer + 5;
                        while (*val == ' ') val++;
                        process_sse_field(current_event, val);
                    }

                    line_pos = 0; // Reset line buffer
                }
                else if (c != '\r' && line_pos < sizeof(line_buffer) - 1)
                {
                    line_buffer[line_pos++] = c;
                }
            }
        }

        esp_http_client_cleanup(client);
        vTaskDelay(pdMS_TO_TICKS(2000)); // Delay before reconnecting
    }
}

// -------------------------------------------------------------------
// CORE 1 LOGIC TASK
// -------------------------------------------------------------------

void solar_algo_task(void *pvParameters)
{
    faikin_data_t state = {0};

    while (1)
    {
        if (xSemaphoreTake(g_telemetry_mutex, pdMS_TO_TICKS(100)) == pdTRUE)
        {
            memcpy(&state, &g_faikin_state, sizeof(faikin_data_t));
            xSemaphoreGive(g_telemetry_mutex);
        }

        if (state.valid)
        {
            ESP_LOGI(TAG_ALGO, "AC Power: %s | Room: %.1f °C | Target: %.1f °C | Mode: %s",
                     state.power ? "ON" : "OFF",
                     state.room_temp,
                     state.target_temp,
                     state.mode);
        }
        else
        {
            ESP_LOGW(TAG_ALGO, "Waiting for initial Faikin telemetry...");
        }

        vTaskDelay(pdMS_TO_TICKS(30000));
    }
}

// -------------------------------------------------------------------
// APP MAIN ENTRY POINT
// -------------------------------------------------------------------

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_flash_init();
    }

    g_telemetry_mutex = xSemaphoreCreateMutex();

    wifi_init_sta();

    xTaskCreatePinnedToCore(sse_comms_task, "sse_comms", 8192, NULL, 5, NULL, 0);
    xTaskCreatePinnedToCore(solar_algo_task, "solar_algo", 4096, NULL, 5, NULL, 1);
}
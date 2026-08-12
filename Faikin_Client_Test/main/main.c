#include <string.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_http_client.h"
#include "cJSON.h"

// --- CONFIGURATION ---
#define EXAMPLE_ESP_WIFI_SSID "DunnysShed"
#define EXAMPLE_ESP_WIFI_PASS "DadsShed"
#define MAXIMUM_RETRY 5

#define FAIKIN_HOST "10.0.0.38" // Faikin IP/Hostname
#define FAIKIN_PORT 80
#define HTTP_RESPONSE_BUFFER_SIZE 2048

static const char *TAG_NET = "NET_CORE0";
static const char *TAG_ALGO = "ALGO_CORE1";

/* Wi-Fi Event Group Bits */
static EventGroupHandle_t s_wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1
static int s_retry_num = 0;

// -------------------------------------------------------------------
// DATA STRUCTURES & INTER-TASK IPC
// -------------------------------------------------------------------

// Shared Telemetry Struct (Protected by Mutex)
typedef struct
{
    float room_temp;
    float target_temp;
    char mode[16];
    char fan_speed[16];
    bool power;
    bool valid; // True if populated with fresh data
} faikin_data_t;

static faikin_data_t g_faikin_state = {0};
static SemaphoreHandle_t g_telemetry_mutex = NULL;

// Command Enumeration
typedef enum
{
    FAIKIN_CMD_SET_POWER,
    FAIKIN_CMD_SET_TEMP,
    FAIKIN_CMD_SET_MODE,
    FAIKIN_CMD_SET_FAN,
    FAIKIN_CMD_SET_SWING,
    FAIKIN_CMD_SET_FULL_STATE
} faikin_cmd_type_t;

// Command Payload Struct
typedef struct
{
    faikin_cmd_type_t cmd_type;
    union
    {
        bool power;
        float target_temp;
        char mode[16];
        char fan_speed[16];
        char swing[16];
        struct
        {
            bool power;
            float target_temp;
            char mode[16];
            char fan_speed[16];
        } full_state;
    } params;
} faikin_command_t;

// Queue for outbound commands (Core 1 -> Core 0)
static QueueHandle_t g_cmd_queue = NULL;

static char response_buffer[HTTP_RESPONSE_BUFFER_SIZE];
static int response_len = 0;

// -------------------------------------------------------------------
// WI-FI & HTTP NETWORK FUNCTIONS (CORE 0)
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
            ESP_LOGI(TAG_NET, "Retrying Wi-Fi connection...");
        }
        else
        {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
    {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG_NET, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
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

esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    switch (evt->event_id)
    {
    case HTTP_EVENT_ON_CONNECTED:
        response_len = 0;
        break;
    case HTTP_EVENT_ON_DATA:
        if (!esp_http_client_is_chunked_response(evt->client))
        {
            if (response_len + evt->data_len < HTTP_RESPONSE_BUFFER_SIZE - 1)
            {
                memcpy(response_buffer + response_len, evt->data, evt->data_len);
                response_len += evt->data_len;
                response_buffer[response_len] = '\0';
            }
        }
        break;
    default:
        break;
    }
    return ESP_OK;
}

static void parse_and_update_state(const char *json_str)
{
    cJSON *root = cJSON_Parse(json_str);
    if (!root)
        return;

    if (xSemaphoreTake(g_telemetry_mutex, pdMS_TO_TICKS(100)) == pdTRUE)
    {
        cJSON *temp = cJSON_GetObjectItem(root, "temp");
        if (cJSON_IsNumber(temp))
            g_faikin_state.room_temp = (float)temp->valuedouble;

        cJSON *stemp = cJSON_GetObjectItem(root, "stemp");
        if (cJSON_IsNumber(stemp))
            g_faikin_state.target_temp = (float)stemp->valuedouble;

        cJSON *mode = cJSON_GetObjectItem(root, "mode");
        if (cJSON_IsString(mode) && mode->valuestring)
            strncpy(g_faikin_state.mode, mode->valuestring, sizeof(g_faikin_state.mode) - 1);

        cJSON *fan = cJSON_GetObjectItem(root, "fan");
        if (cJSON_IsString(fan) && fan->valuestring)
            strncpy(g_faikin_state.fan_speed, fan->valuestring, sizeof(g_faikin_state.fan_speed) - 1);

        cJSON *power = cJSON_GetObjectItem(root, "power");
        if (cJSON_IsBool(power))
            g_faikin_state.power = cJSON_IsTrue(power);

        g_faikin_state.valid = true;
        xSemaphoreGive(g_telemetry_mutex);
    }
    cJSON_Delete(root);
}

esp_err_t faikin_post_json(const char *json_body)
{
    esp_http_client_config_t config = {
        .host = FAIKIN_HOST,
        .port = FAIKIN_PORT,
        .path = "/faikin",
        .method = HTTP_METHOD_POST,
        .timeout_ms = 5000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client)
        return ESP_FAIL;

    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, json_body, strlen(json_body));

    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK)
    {
        ESP_LOGI(TAG_NET, "Command sent to Faikin successfully");
    }
    else
    {
        ESP_LOGE(TAG_NET, "HTTP POST failed: %s", esp_err_to_name(err));
    }

    esp_http_client_cleanup(client);
    return err;
}

esp_err_t faikin_execute_command(const faikin_command_t *cmd)
{
    cJSON *root = cJSON_CreateObject();
    if (!root)
        return ESP_ERR_NO_MEM;

    switch (cmd->cmd_type)
    {
    case FAIKIN_CMD_SET_POWER:
        cJSON_AddBoolToObject(root, "power", cmd->params.power);
        break;
    case FAIKIN_CMD_SET_TEMP:
        cJSON_AddNumberToObject(root, "stemp", cmd->params.target_temp);
        break;
    case FAIKIN_CMD_SET_MODE:
        cJSON_AddStringToObject(root, "mode", cmd->params.mode);
        break;
    case FAIKIN_CMD_SET_FAN:
        cJSON_AddStringToObject(root, "fan", cmd->params.fan_speed);
        break;
    case FAIKIN_CMD_SET_SWING:
        cJSON_AddStringToObject(root, "swing", cmd->params.swing);
        break;
    case FAIKIN_CMD_SET_FULL_STATE:
        cJSON_AddBoolToObject(root, "power", cmd->params.full_state.power);
        cJSON_AddNumberToObject(root, "stemp", cmd->params.full_state.target_temp);
        cJSON_AddStringToObject(root, "mode", cmd->params.full_state.mode);
        cJSON_AddStringToObject(root, "fan", cmd->params.full_state.fan_speed);
        break;
    default:
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    char *json_string = cJSON_PrintUnformatted(root);
    esp_err_t err = faikin_post_json(json_string);

    cJSON_free(json_string);
    cJSON_Delete(root);
    return err;
}

// -------------------------------------------------------------------
// TASKS
// -------------------------------------------------------------------

// CORE 0 TASK: Handles network traffic & HTTP requests
void comms_task(void *pvParameters)
{
    esp_http_client_config_t get_config = {
        .host = FAIKIN_HOST,
        .port = FAIKIN_PORT,
        .path = "/faikin",
        .event_handler = http_event_handler,
        .timeout_ms = 5000,
    };

    esp_http_client_handle_t get_client = esp_http_client_init(&get_config);
    faikin_command_t queued_cmd;
    
    // Trigger first poll immediately upon boot
    TickType_t last_poll_time = xTaskGetTickCount() - pdMS_TO_TICKS(10000);

    while (1)
    {
        // 1. Process outbound commands from Core 1 Queue
        while (xQueueReceive(g_cmd_queue, &queued_cmd, 0) == pdTRUE)
        {
            ESP_LOGI(TAG_NET, "Dequeueing command sent from Core 1...");
            faikin_execute_command(&queued_cmd);
        }

        // 2. Poll Faikin GET endpoint every 10 seconds
        TickType_t now = xTaskGetTickCount();
        if ((now - last_poll_time) >= pdMS_TO_TICKS(10000))
        {
            ESP_LOGI(TAG_NET, "Polling Faikin status at %s...", FAIKIN_HOST);
            response_len = 0;
            memset(response_buffer, 0, sizeof(response_buffer));

            esp_err_t err = esp_http_client_perform(get_client);
            if (err == ESP_OK)
            {
                int status_code = esp_http_client_get_status_code(get_client);
                if (status_code == 200)
                {
                    parse_and_update_state(response_buffer);
                    ESP_LOGI(TAG_NET, "Successfully parsed Faikin state.");
                }
                else
                {
                    ESP_LOGW(TAG_NET, "Faikin returned HTTP status: %d", status_code);
                }
            }
            else
            {
                ESP_LOGE(TAG_NET, "Failed to connect to Faikin: %s", esp_err_to_name(err));
            }
            last_poll_time = now;
        }

        vTaskDelay(pdMS_TO_TICKS(100)); // Yield CPU0
    }

    esp_http_client_cleanup(get_client);
    vTaskDelete(NULL);
}

// CORE 1 TASK: Placeholder for Solar Logic & Control Calculations
void solar_algo_task(void *pvParameters)
{
    faikin_data_t current_telemetry = {0};

    while (1)
    {
        if (xSemaphoreTake(g_telemetry_mutex, pdMS_TO_TICKS(50)) == pdTRUE)
        {
            memcpy(&current_telemetry, &g_faikin_state, sizeof(faikin_data_t));
            xSemaphoreGive(g_telemetry_mutex);
        }

        if (current_telemetry.valid)
        {
            ESP_LOGI(TAG_ALGO, "Evaluating logic. AC Power: %s | Room Temp: %.1f °C",
                     current_telemetry.power ? "ON" : "OFF", current_telemetry.room_temp);
        }
        else
        {
            ESP_LOGW(TAG_ALGO, "Waiting for valid Faikin telemetry...");
        }

        vTaskDelay(pdMS_TO_TICKS(5000));
    }

    vTaskDelete(NULL);
}

// -------------------------------------------------------------------
// MAIN ENTRY POINT
// -------------------------------------------------------------------

void app_main(void)
{
    // 1. Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_flash_init();
    }

    // 2. Create IPC Sync Primitives
    g_telemetry_mutex = xSemaphoreCreateMutex();
    g_cmd_queue = xQueueCreate(10, sizeof(faikin_command_t)); // Queue depth of 10

    // 3. Connect Wi-Fi
    wifi_init_sta();

    // 4. Pin Comms Task to CORE 0
    xTaskCreatePinnedToCore(
        comms_task,
        "comms_task",
        8192,
        NULL,
        5,
        NULL,
        0 // Core 0 (PRO_CPU)
    );

    // 5. Pin Solar Algorithm Task to CORE 1
    xTaskCreatePinnedToCore(
        solar_algo_task,
        "solar_algo_task",
        4096,
        NULL,
        5,
        NULL,
        1 // Core 1 (APP_CPU)
    );
}
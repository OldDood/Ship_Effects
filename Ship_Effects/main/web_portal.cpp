#include "web_portal.hpp"
#include "esp_http_server.h"
#include "esp_log.h"
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include "esp_netif.h"
#include "esp_vfs_fat.h"
#include <time.h>

// This allows web_portal to call the NVS save function located in main.cpp
extern "C" void save_volume_to_nvs(uint8_t vol);

extern const uint8_t ship_web_html_start[] asm("_binary_ship_web_html_start");
extern const uint8_t ship_web_html_end[] asm("_binary_ship_web_html_end");

static const char *TAG = "WebPortal";
httpd_handle_t server = NULL;

// --- External Solar Functions ---
extern "C" bool is_solar_night(struct tm *ti);
extern "C" int get_sunrise_mins();
extern "C" int get_sunset_mins();

// In a header file included by both main.cpp and web_portal.cpp
typedef struct
{
    char filename[64];
    bool sync_enabled;
} playback_cmd_t;

// Top of web_portal.cpp
extern volatile bool AUDIO_ENABLED;
extern void save_play_state_to_nvs(bool state);

esp_err_t play_start_handler(httpd_req_t *req)
{
    AUDIO_ENABLED = true;
    save_play_state_to_nvs(true);
    ESP_LOGI("WEB", "Audio Relay: CLOSED (On)");
    httpd_resp_sendstr(req, "Audio Enabled");
    return ESP_OK;
}

esp_err_t play_stop_handler(httpd_req_t *req)
{
    AUDIO_ENABLED = false;
    save_play_state_to_nvs(false);
    ESP_LOGW("WEB", "Audio Relay: OPEN (Off)");
    httpd_resp_sendstr(req, "Audio Disabled");
    return ESP_OK;
}

// This allows web_portal to call the function located in main.cpp
extern "C" void trigger_project_play(playback_cmd_t *cmd);

// --- Handlers ---

esp_err_t index_get_handler(httpd_req_t *req)
{
    // 2. Use the symbols directly
    const char *data = (const char *)ship_web_html_start;
    size_t len = ship_web_html_end - ship_web_html_start;

    // 3. Keep your diagnostic logs to verify the update
    ESP_LOGW("DEBUG", "===> ROOT HANDLER TRIGGERED <===");
    ESP_LOGW("DEBUG", "Address of data: %p | Size: %zu bytes", (void *)ship_web_html_start, len);

    // This will tell you immediately if you're looking at the new code
    // Look for your new <input type="range"> or functions in the logs
    ESP_LOGI(TAG, "First 20 bytes: %.20s", data);

    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, data, len);
}

esp_err_t list_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr_chunk(req, "{\"files\": [");
    DIR *dir = opendir("/sdcard");
    bool first = true;
    if (dir != NULL)
    {
        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL)
        {
            if (ent->d_type == DT_REG)
            {
                char filepath[1024];
                struct stat st;
                snprintf(filepath, sizeof(filepath), "/sdcard/%s", ent->d_name);
                long size = 0;
                if (stat(filepath, &st) == 0)
                    size = st.st_size;

                char entry_str[400];
                snprintf(entry_str, sizeof(entry_str), "%s{\"name\":\"%s\",\"size\":%ld}",
                         first ? "" : ",", ent->d_name, size);
                httpd_resp_sendstr_chunk(req, entry_str);
                first = false;
            }
        }
        closedir(dir);
    }
    httpd_resp_sendstr_chunk(req, "]}");
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

esp_err_t upload_post_handler(httpd_req_t *req)
{
    char filepath[300];
    // We expect the filename in the query string: /upload?file=filename.mp3
    char query[256], filename[256];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK)
    {
        if (httpd_query_key_value(query, "file", filename, sizeof(filename)) == ESP_OK)
        {
            snprintf(filepath, sizeof(filepath), "/sdcard/%s", filename);
        }
        else
        {
            return httpd_resp_send_404(req);
        }
    }
    else
    {
        return httpd_resp_send_404(req);
    }

    FILE *fd = fopen(filepath, "wb");
    if (!fd)
    {
        ESP_LOGE(TAG, "Failed to create file : %s", filepath);
        return httpd_resp_send_500(req);
    }

    char *buf = (char *)malloc(4096);
    int received;
    while ((received = httpd_req_recv(req, buf, 4096)) > 0)
    {
        fwrite(buf, 1, received, fd);
    }
    fclose(fd);
    free(buf);

    httpd_resp_sendstr(req, "File uploaded successfully");
    return ESP_OK;
}

esp_err_t download_get_handler(httpd_req_t *req)
{
    char filepath[300], query[256], filename[256];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK)
    {
        if (httpd_query_key_value(query, "file", filename, sizeof(filename)) == ESP_OK)
        {
            snprintf(filepath, sizeof(filepath), "/sdcard/%s", filename);
        }
        else
            return httpd_resp_send_404(req);
    }
    else
        return httpd_resp_send_404(req);

    FILE *f = fopen(filepath, "rb");
    if (f == NULL)
        return httpd_resp_send_404(req);

    // Set to audio/mpeg for MP3 effects
    httpd_resp_set_type(req, "audio/mpeg");

    const size_t chunk_size = 16384;
    char *buffer = (char *)malloc(chunk_size);
    if (buffer == NULL)
    {
        fclose(f);
        return httpd_resp_send_500(req);
    }

    size_t read_bytes;
    esp_err_t res = ESP_OK;
    while ((read_bytes = fread(buffer, 1, chunk_size, f)) > 0)
    {
        res = httpd_resp_send_chunk(req, buffer, read_bytes);
        if (res != ESP_OK)
            break;
    }

    fclose(f);
    free(buffer);

    if (res == ESP_OK)
    {
        httpd_resp_send_chunk(req, NULL, 0);
    }
    return res;
}

esp_err_t delete_get_handler(httpd_req_t *req)
{
    char query[256], filename[256], filepath[300];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK)
    {
        if (httpd_query_key_value(query, "file", filename, sizeof(filename)) == ESP_OK)
        {
            snprintf(filepath, sizeof(filepath), "/sdcard/%s", filename);
            unlink(filepath);
            return httpd_resp_sendstr(req, "Deleted");
        }
    }
    return httpd_resp_send_404(req);
}

esp_err_t delete_all_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Web request: Delete All Files");
    DIR *dir = opendir("/sdcard");
    if (dir == NULL)
        return httpd_resp_send_500(req);

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL)
    {
        if (ent->d_type == DT_REG)
        {
            char filepath[300];
            snprintf(filepath, sizeof(filepath), "/sdcard/%s", ent->d_name);
            unlink(filepath);
        }
    }
    closedir(dir);
    return httpd_resp_sendstr(req, "All deleted");
}

esp_err_t storage_get_handler(httpd_req_t *req)
{
    FATFS *fs;
    DWORD fre_clust;
    FRESULT res = f_getfree("0:", &fre_clust, &fs);

    uint64_t total_bytes = 0;
    uint64_t free_bytes = 0;

    if (res == FR_OK)
    {
        uint64_t tot_sect = (uint64_t)(fs->n_fatent - 2) * fs->csize;
        uint64_t fre_sect = (uint64_t)fre_clust * fs->csize;
        total_bytes = tot_sect * 512;
        free_bytes = fre_sect * 512;
    }

    char json_response[128];
    snprintf(json_response, sizeof(json_response),
             "{\"total\":%llu,\"free\":%llu}", total_bytes, free_bytes);

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, json_response);
}

esp_err_t set_default_handler(httpd_req_t *req)
{
    char query[256], filename[256];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK)
    {
        if (httpd_query_key_value(query, "file", filename, sizeof(filename)) == ESP_OK)
        {
            // Write the filename to autoplay.txt on the SD card
            FILE *f = fopen("/sdcard/autoplay.txt", "w");
            if (f == NULL)
            {
                ESP_LOGE(TAG, "Failed to open autoplay.txt for writing");
                return httpd_resp_send_500(req);
            }
            fprintf(f, "%s", filename);
            fclose(f);

            ESP_LOGI(TAG, "Default song saved to SD card: %s", filename);
            return httpd_resp_sendstr(req, "Default song saved successfully");
        }
    }
    return httpd_resp_send_404(req);
}

esp_err_t solar_get_handler(httpd_req_t *req)
{
    struct tm ti;
    time_t now;
    char json_response[128];

    time(&now);
    localtime_r(&now, &ti);

    if (ti.tm_year < (2020 - 1900))
    {
        snprintf(json_response, sizeof(json_response), "{\"error\":\"Time not synced via NTP\"}");
    }
    else
    {
        int sunrise = get_sunrise_mins();
        int sunset = get_sunset_mins();
        bool night = is_solar_night(&ti);

        snprintf(json_response, sizeof(json_response),
                 "{\"sunrise\":\"%02d:%02d\",\"sunset\":\"%02d:%02d\",\"is_night\":%s}",
                 sunrise / 60, sunrise % 60,
                 sunset / 60, sunset % 60,
                 night ? "true" : "false");
    }

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, json_response);
}

esp_err_t play_get_handler(httpd_req_t *req)
{
    char query[256];
    // Create the struct we defined in main.h
    playback_cmd_t cmd;
    memset(&cmd, 0, sizeof(playback_cmd_t));

    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK)
    {
        // 1. Extract "project" name from URL into the struct
        if (httpd_query_key_value(query, "project", cmd.filename, sizeof(cmd.filename)) == ESP_OK)
        {
            // 2. Extract "sync" preference
            char sync_flag[10] = {0};
            if (httpd_query_key_value(query, "sync", sync_flag, sizeof(sync_flag)) == ESP_OK)
            {
                cmd.sync_enabled = (strcmp(sync_flag, "1") == 0);
            }

            ESP_LOGI(TAG, "Web Request Received -> File: %s, Sync: %d", cmd.filename, cmd.sync_enabled);

            // 3. SEND TO QUEUE: This is the critical line that wakes up Core 1
            trigger_project_play(&cmd);

            httpd_resp_sendstr(req, "Play command received");
            return ESP_OK;
        }
    }
    return httpd_resp_send_404(req);
}

// External function located in main.cpp
extern "C" void set_master_volume(int vol_percent);

esp_err_t volume_get_handler(httpd_req_t *req)
{
    char query[256];
    char vol_str[10];
    static int last_saved_vol = -1; // Local latch to prevent redundant flash writes

    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK)
    {
        if (httpd_query_key_value(query, "val", vol_str, sizeof(vol_str)) == ESP_OK)
        {
            int vol = atoi(vol_str);

            // 1. Apply the volume to the hardware immediately for responsiveness
            set_master_volume(vol);

            // 2. Save to NVRAM only if the value has changed
            if (vol != last_saved_vol)
            {
                save_volume_to_nvs((uint8_t)vol);
                last_saved_vol = vol;
            }

            char resp[32];
            snprintf(resp, sizeof(resp), "Volume set to %d%%", vol);
            httpd_resp_sendstr(req, resp);
            return ESP_OK;
        }
    }
    return httpd_resp_send_404(req);
}

// --- Server Control ---

void start_web_portal()
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 15; //
    config.stack_size = 10240;
    config.lru_purge_enable = true;
    config.recv_wait_timeout = 10;

    if (httpd_start(&server, &config) == ESP_OK)
    {
        httpd_uri_t index_uri = {.uri = "/", .method = HTTP_GET, .handler = index_get_handler, .user_ctx = NULL};
        httpd_register_uri_handler(server, &index_uri);

        httpd_uri_t list_uri = {.uri = "/list", .method = HTTP_GET, .handler = list_get_handler, .user_ctx = NULL};
        httpd_register_uri_handler(server, &list_uri);

        httpd_uri_t upload_uri = {.uri = "/upload", .method = HTTP_POST, .handler = upload_post_handler, .user_ctx = NULL};
        httpd_register_uri_handler(server, &upload_uri);

        httpd_uri_t download_uri = {.uri = "/download", .method = HTTP_GET, .handler = download_get_handler, .user_ctx = NULL};
        httpd_register_uri_handler(server, &download_uri);

        httpd_uri_t delete_uri = {.uri = "/delete", .method = HTTP_GET, .handler = delete_get_handler, .user_ctx = NULL};
        httpd_register_uri_handler(server, &delete_uri);

        httpd_uri_t delall_uri = {.uri = "/delete_all", .method = HTTP_GET, .handler = delete_all_handler, .user_ctx = NULL};
        httpd_register_uri_handler(server, &delall_uri);

        httpd_uri_t storage_uri = {.uri = "/storage", .method = HTTP_GET, .handler = storage_get_handler, .user_ctx = NULL};
        httpd_register_uri_handler(server, &storage_uri);

        httpd_uri_t solar_uri = {.uri = "/solar", .method = HTTP_GET, .handler = solar_get_handler, .user_ctx = NULL};
        httpd_register_uri_handler(server, &solar_uri);

        httpd_uri_t play_uri = {.uri = "/play", .method = HTTP_GET, .handler = play_get_handler, .user_ctx = NULL};
        httpd_register_uri_handler(server, &play_uri);

        httpd_uri_t set_default_uri = {.uri = "/set_default", .method = HTTP_GET, .handler = set_default_handler, .user_ctx = NULL};
        httpd_register_uri_handler(server, &set_default_uri);

        httpd_uri_t volume_uri = {.uri = "/volume", .method = HTTP_GET, .handler = volume_get_handler, .user_ctx = NULL};
        httpd_register_uri_handler(server, &volume_uri);

        httpd_uri_t start_uri = {.uri = "/start", .method = HTTP_GET, .handler = play_start_handler};
        httpd_register_uri_handler(server, &start_uri);

        httpd_uri_t stop_uri = {.uri = "/stop", .method = HTTP_GET, .handler = play_stop_handler};
        httpd_register_uri_handler(server, &stop_uri);

        ESP_LOGI(TAG, "Server started with all URIs initialized.");

        ESP_LOGI(TAG, "Server started with Play Sync URI initialized.");
    }
}

void stop_web_portal()
{
    if (server != NULL)
    {
        if (httpd_stop(server) == ESP_OK)
        {
            server = NULL;
            ESP_LOGI(TAG, "Server stopped successfully.");
        }
        else
        {
            ESP_LOGE(TAG, "Failed to stop the server.");
        }
    }
}
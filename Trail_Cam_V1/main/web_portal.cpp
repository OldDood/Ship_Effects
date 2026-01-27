#include "web_portal.hpp"
#include "esp_http_server.h"
#include "esp_log.h"
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include "esp_netif.h" 
#include "esp_vfs_fat.h"

extern const uint8_t web_portal_html_start[] asm("_binary_web_portal_html_start");
extern const uint8_t web_portal_html_end[]   asm("_binary_web_portal_html_end");
static const char *TAG = "WebPortal";

// Link to the function in main.cpp
extern "C" esp_err_t take_photo(); 

// --- Handlers ---

esp_err_t index_get_handler(httpd_req_t *req) {
    const size_t html_len = web_portal_html_end - web_portal_html_start;
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, (const char *)web_portal_html_start, html_len);
}

esp_err_t list_get_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr_chunk(req, "{\"files\": [");
    DIR *dir = opendir("/sdcard");
    bool first = true;
    if (dir != NULL) {
        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL) {
            if (ent->d_type == DT_REG) {
                char filepath[300];
                struct stat st;
                snprintf(filepath, sizeof(filepath), "/sdcard/%s", ent->d_name);
                long size = 0;
                if (stat(filepath, &st) == 0) size = st.st_size;
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

esp_err_t download_get_handler(httpd_req_t *req) {
    char filepath[300], query[256], filename[256];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        if (httpd_query_key_value(query, "file", filename, sizeof(filename)) == ESP_OK) {
            snprintf(filepath, sizeof(filepath), "/sdcard/%s", filename);
        } else return httpd_resp_send_404(req);
    } else return httpd_resp_send_404(req);

    FILE* f = fopen(filepath, "rb");
    if (f == NULL) return httpd_resp_send_404(req);
    
    httpd_resp_set_type(req, "image/jpeg");

    const size_t chunk_size = 16384; 
    char *buffer = (char*)malloc(chunk_size);
    if (buffer == NULL) {
        fclose(f);
        return httpd_resp_send_500(req);
    }

    size_t read_bytes;
    esp_err_t res = ESP_OK;
    while ((read_bytes = fread(buffer, 1, chunk_size, f)) > 0) {
        res = httpd_resp_send_chunk(req, buffer, read_bytes);
        if (res != ESP_OK) break; 
    }

    fclose(f);
    free(buffer);

    if (res == ESP_OK) {
        httpd_resp_send_chunk(req, NULL, 0); 
    }
    return res;
}

esp_err_t delete_get_handler(httpd_req_t *req) {
    char query[256], filename[256], filepath[300];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        if (httpd_query_key_value(query, "file", filename, sizeof(filename)) == ESP_OK) {
            snprintf(filepath, sizeof(filepath), "/sdcard/%s", filename);
            unlink(filepath);
            return httpd_resp_sendstr(req, "Deleted");
        }
    }
    return httpd_resp_send_404(req);
}

esp_err_t capture_get_handler(httpd_req_t *req) {
    ESP_LOGI(TAG, "Web request: /capture");
    if (take_photo() == ESP_OK) {
        return httpd_resp_sendstr(req, "Capture OK");
    } else {
        return httpd_resp_send_500(req);
    }
}

esp_err_t delete_all_handler(httpd_req_t *req) {
    ESP_LOGI(TAG, "Web request: Delete All Files");
    DIR *dir = opendir("/sdcard");
    if (dir == NULL) return httpd_resp_send_500(req);

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (ent->d_type == DT_REG) {
            char filepath[300];
            snprintf(filepath, sizeof(filepath), "/sdcard/%s", ent->d_name);
            unlink(filepath);
        }
    }
    closedir(dir);
    return httpd_resp_sendstr(req, "All deleted");
}

esp_err_t storage_get_handler(httpd_req_t *req) {
    FATFS *fs;
    DWORD fre_clust;
    FRESULT res = f_getfree("0:", &fre_clust, &fs);
    
    uint64_t total_bytes = 0;
    uint64_t free_bytes = 0;

    if (res == FR_OK) {
        // Use uint64_t to prevent overflow on larger SD cards
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

// --- Server Start ---

void start_web_portal() {
    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 10240; 
    config.lru_purge_enable = true; 
    config.recv_wait_timeout = 10;

    if (httpd_start(&server, &config) == ESP_OK) {
        // Every URI struct must have .user_ctx explicitly initialized to NULL
        httpd_uri_t index_uri = { .uri = "/", .method = HTTP_GET, .handler = index_get_handler, .user_ctx = NULL };
        httpd_register_uri_handler(server, &index_uri);
        
        httpd_uri_t list_uri = { .uri = "/list", .method = HTTP_GET, .handler = list_get_handler, .user_ctx = NULL };
        httpd_register_uri_handler(server, &list_uri);
        
        httpd_uri_t download_uri = { .uri = "/download", .method = HTTP_GET, .handler = download_get_handler, .user_ctx = NULL };
        httpd_register_uri_handler(server, &download_uri);
        
        httpd_uri_t delete_uri = { .uri = "/delete", .method = HTTP_GET, .handler = delete_get_handler, .user_ctx = NULL };
        httpd_register_uri_handler(server, &delete_uri);

        httpd_uri_t capture_uri = { .uri = "/capture", .method = HTTP_GET, .handler = capture_get_handler, .user_ctx = NULL };
        httpd_register_uri_handler(server, &capture_uri);
        
        httpd_uri_t delall_uri = { .uri = "/delete_all", .method = HTTP_GET, .handler = delete_all_handler, .user_ctx = NULL };
        httpd_register_uri_handler(server, &delall_uri);

        httpd_uri_t storage_uri = { .uri = "/storage", .method = HTTP_GET, .handler = storage_get_handler, .user_ctx = NULL };
        httpd_register_uri_handler(server, &storage_uri);

        ESP_LOGI(TAG, "Server started with all URIs initialized.");
    }
}
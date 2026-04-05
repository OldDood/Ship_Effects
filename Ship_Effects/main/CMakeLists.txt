idf_component_register(
    SRCS "web_portal.cpp" "main.cpp"
    INCLUDE_DIRS "."
    REQUIRES fatfs sdmmc esp_wifi esp_event nvs_flash esp_http_server
    EMBED_FILES "web_portal.html"
)
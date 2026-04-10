idf_component_register(
    SRCS "web_portal.cpp" "main.cpp"
    INCLUDE_DIRS "."
    REQUIRES esp_driver_i2s fatfs sdmmc esp_wifi esp_event nvs_flash esp_http_server esp_driver_uart driver
    EMBED_FILES "ship_web.html" 
)
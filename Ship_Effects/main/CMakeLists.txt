# Updated CMakeLists.txt for the main component
idf_component_register(
    SRCS 
        "ota.c" 
        "web_portal.cpp" 
        "main.cpp"
    INCLUDE_DIRS "."
    REQUIRES 
        esp_driver_i2s 
        fatfs 
        sdmmc 
        esp_wifi 
        esp_event 
        nvs_flash 
        esp_http_server 
        esp_driver_uart 
        driver
        app_update
    EMBED_FILES 
        "ship_web.html" 
)
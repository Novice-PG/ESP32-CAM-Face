idf_component_register(
    SRCS "main.c"
         "camera_init.c"
         "face_detect.c"
         "face_db.c"
         "web_server.c"
    INCLUDE_DIRS "."
    REQUIRES esp_http_server esp_timer esp_wifi esp_event nvs_flash esp_psram esp_partition driver spiffs esp32-camera fatfs sdmmc
)

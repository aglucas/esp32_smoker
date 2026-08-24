idf_component_register(
    SRCS "main.c" "wifi_manager.c" "temp_sensor.c" "sd_logger.c" "web_server.c"
    INCLUDE_DIRS "."
    EMBED_TXTFILES "index.html"
    PRIV_REQUIRES esp_wifi esp_event esp_netif nvs_flash esp_http_server esp_adc driver fatfs sdmmc esp_timer json lwip
)

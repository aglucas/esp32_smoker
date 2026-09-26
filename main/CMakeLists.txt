idf_component_register(
    SRCS "main.c" "wifi_manager.c" "temp_sensor.c" "data_log.c" "web_server.c" "alerts.c" "cook_plan.c"
    INCLUDE_DIRS "."
    EMBED_TXTFILES "index.html"
    PRIV_REQUIRES esp_wifi esp_event esp_netif nvs_flash esp_http_server esp_http_client mbedtls esp_adc esp_timer json lwip
)

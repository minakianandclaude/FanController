idf_component_register(
    SRCS "api.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_server fan_control event_emitter ota json app_update led_control wifi esp_wifi
)

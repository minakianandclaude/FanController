idf_component_register(
    SRCS "event_emitter.c"
    INCLUDE_DIRS "include"
    REQUIRES fan_control esp_http_server
)

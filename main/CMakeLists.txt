idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES ota nvs_flash spi_flash bts7960 buttons fan_control event_emitter wifi api status_led ble_prov
)

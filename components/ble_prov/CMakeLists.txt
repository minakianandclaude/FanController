idf_component_register(
    SRCS "ble_prov.c"
    INCLUDE_DIRS "include"
    REQUIRES wifi_provisioning esp_event bt status_led
)

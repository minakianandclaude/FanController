idf_component_register(
    SRCS "fan_control.c"
    INCLUDE_DIRS "include"
    REQUIRES bts7960 buttons settings
)

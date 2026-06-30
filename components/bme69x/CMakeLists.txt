idf_component_register(
    SRCS "bme69x.c" "bme69x_port.c"
    INCLUDE_DIRS "."
    REQUIRES driver
    PRIV_REQUIRES esp_common esp_timer
)
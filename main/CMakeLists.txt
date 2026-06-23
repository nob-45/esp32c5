idf_component_register(SRCS "station_example_main.c"
                    PRIV_REQUIRES esp_wifi nvs_flash mqtt esp_driver_i2c esp_event esp_netif lwip mbedtls volc_conv
                    INCLUDE_DIRS ".")

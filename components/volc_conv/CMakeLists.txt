idf_component_register(
    SRCS
        "src/volc_conv_register.c"
        "src/volc_conv_ws.c"
        "src/volc_conv_audio.c"
        "src/volc_conv_app.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
    REQUIRES
        esp_event
        esp_websocket_client
        esp_http_client
        esp-tls
        esp_adc
        esp_driver_ledc
        esp_driver_gptimer
        esp_ringbuf
        nvs_flash
        mbedtls
        json
)
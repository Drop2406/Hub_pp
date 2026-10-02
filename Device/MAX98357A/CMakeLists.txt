idf_component_register(
    SRCS "audio_output.c"
    INCLUDE_DIRS "."
    REQUIRES
        esp_driver_i2s
        esp_driver_gpio
        freertos
)
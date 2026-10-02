idf_component_register(
    SRCS "app_voice.c"
    INCLUDE_DIRS "."
    REQUIRES
        app_media
        app_led
        INMP441
        SD_CARD
        espressif__esp-sr
        esp_timer
        freertos
)
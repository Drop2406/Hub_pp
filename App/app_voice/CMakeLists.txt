idf_component_register(
    SRCS
        "app_voice.c"
        "voice_recorder.c"
    INCLUDE_DIRS "."
    REQUIRES
        app_media
        app_led
        INMP441
        SD_CARD
        Groq
        espressif__esp-sr
        esp_timer
        freertos
        heap
)
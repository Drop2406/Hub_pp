idf_component_register(
    SRCS "app_media.c"
    INCLUDE_DIRS "."
    REQUIRES
        AVI
        JPEG
        SSD1351
        MAX98357A
        heap
        esp_timer
)
idf_component_register(
    SRCS "image_decoder.c"
    INCLUDE_DIRS "."
    REQUIRES
        espressif__esp_jpeg
)
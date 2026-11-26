idf_component_register(
    SRCS
        "groq_stt.c"
        "groq_chat.c"
        "groq_tts.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES
        esp_http_client
        json
        mbedtls
        heap
        MAX98357A
)
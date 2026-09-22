idf_component_register(SRCS "wake_word.c"
                    INCLUDE_DIRS "include"
                    REQUIRES "audio" "esp_timer" "freertos"  "log")
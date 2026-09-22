idf_component_register(SRCS "wake_word_manager.c"
                    INCLUDE_DIRS "include"
                    REQUIRES "wake_word" "animation_manager" "esp_timer" "freertos" "log")
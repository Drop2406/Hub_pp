#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"


esp_err_t audio_output_init(void);

esp_err_t audio_output_test_tone(void);

esp_err_t audio_output_enqueue_pcm_mono16(
    const uint8_t *pcm_data,
    size_t pcm_size
);
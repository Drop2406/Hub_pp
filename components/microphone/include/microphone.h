#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"


#define MICROPHONE_SAMPLE_RATE 16000U


esp_err_t microphone_init(void);

esp_err_t microphone_read(
    int16_t *samples,
    size_t sample_count,
    size_t *samples_read
);

esp_err_t microphone_deinit(void);
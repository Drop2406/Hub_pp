#ifndef GROQ_STT_H
#define GROQ_STT_H

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

esp_err_t groq_stt_init(const char *api_key);

esp_err_t groq_stt_transcribe_pcm16(
    const int16_t *samples,
    size_t sample_count,
    uint32_t sample_rate,
    char *text_buffer,
    size_t text_capacity
);

#endif
#pragma once

#include "esp_err.h"

esp_err_t groq_tts_init(const char *api_key);

/*
 * Convert text to speech with Groq Orpheus and stream
 * returned WAV PCM directly to audio_output.
 */
esp_err_t groq_tts_speak(const char *text);
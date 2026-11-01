#ifndef VOICE_RECORDER_H
#define VOICE_RECORDER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"


#define VOICE_RECORDER_SAMPLE_RATE 16000U


typedef struct {
    const int16_t *samples;
    size_t sample_count;
    uint32_t sample_rate;
} voice_recording_t;


/*
 * Allocate the maximum PCM16 mono recording buffer in PSRAM.
 */
esp_err_t voice_recorder_init(
    uint32_t max_duration_ms
);


/*
 * Reset the current recording and start capturing.
 */
esp_err_t voice_recorder_start(void);


/*
 * Copy microphone PCM into the PSRAM recording buffer.
 *
 * ESP_ERR_NO_MEM means the configured recording buffer
 * has just become full. The samples up to capacity are
 * already stored safely.
 */
esp_err_t voice_recorder_push(
    const int16_t *samples,
    size_t sample_count
);


/*
 * Return a view of the current recording.
 *
 * The returned samples remain owned by voice_recorder.
 * Do not free the pointer.
 */
esp_err_t voice_recorder_stop(
    voice_recording_t *recording
);


bool voice_recorder_is_recording(void);


void voice_recorder_deinit(void);


#endif
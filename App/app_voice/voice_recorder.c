#include "voice_recorder.h"

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"


static const char *TAG = "VOICE_RECORDER";


static int16_t *s_samples = NULL;

static size_t s_capacity_samples = 0U;

static atomic_size_t s_sample_count =
    ATOMIC_VAR_INIT(0U);

static atomic_bool s_recording =
    ATOMIC_VAR_INIT(false);


esp_err_t voice_recorder_init(
    uint32_t max_duration_ms
)
{
    if (
        max_duration_ms == 0U ||
        s_samples != NULL
    ) {
        return ESP_ERR_INVALID_STATE;
    }


    const uint64_t sample_count_64 =
        (
            (uint64_t)VOICE_RECORDER_SAMPLE_RATE *
            (uint64_t)max_duration_ms
        ) /
        1000ULL;


    if (
        sample_count_64 == 0ULL ||
        sample_count_64 > SIZE_MAX
    ) {
        return ESP_ERR_INVALID_SIZE;
    }


    s_capacity_samples =
        (size_t)sample_count_64;


    const size_t buffer_bytes =
        s_capacity_samples *
        sizeof(int16_t);


    s_samples =
        heap_caps_malloc(
            buffer_bytes,
            MALLOC_CAP_SPIRAM |
            MALLOC_CAP_8BIT
        );


    if (s_samples == NULL) {

        s_capacity_samples = 0U;

        ESP_LOGE(
            TAG,
            "Cannot allocate recording buffer in PSRAM"
        );

        return ESP_ERR_NO_MEM;
    }


    atomic_store(
        &s_sample_count,
        0U
    );

    atomic_store(
        &s_recording,
        false
    );


    ESP_LOGI(
        TAG,
        "PSRAM recording buffer ready: %u samples, %u bytes",
        (unsigned int)s_capacity_samples,
        (unsigned int)buffer_bytes
    );


    return ESP_OK;
}


esp_err_t voice_recorder_start(void)
{
    if (
        s_samples == NULL ||
        s_capacity_samples == 0U
    ) {
        return ESP_ERR_INVALID_STATE;
    }


    /*
     * Publish a clean sample count before enabling capture.
     */
    atomic_store(
        &s_sample_count,
        0U
    );

    atomic_store(
        &s_recording,
        true
    );


    ESP_LOGI(
        TAG,
        "Recording started"
    );


    return ESP_OK;
}


esp_err_t voice_recorder_push(
    const int16_t *samples,
    size_t sample_count
)
{
    if (
        samples == NULL ||
        sample_count == 0U
    ) {
        return ESP_ERR_INVALID_ARG;
    }


    if (
        s_samples == NULL ||
        s_capacity_samples == 0U
    ) {
        return ESP_ERR_INVALID_STATE;
    }


    if (
        !atomic_load(
            &s_recording
        )
    ) {
        return ESP_ERR_INVALID_STATE;
    }


    size_t current_count =
        atomic_load(
            &s_sample_count
        );


    if (
        current_count >=
        s_capacity_samples
    ) {

        atomic_store(
            &s_recording,
            false
        );

        return ESP_ERR_NO_MEM;
    }


    const size_t remaining =
        s_capacity_samples -
        current_count;


    const size_t samples_to_copy =
        sample_count < remaining
        ? sample_count
        : remaining;


    memcpy(
        &s_samples[current_count],
        samples,
        samples_to_copy *
            sizeof(int16_t)
    );


    current_count +=
        samples_to_copy;


    atomic_store(
        &s_sample_count,
        current_count
    );


    if (
        current_count >=
        s_capacity_samples
    ) {

        atomic_store(
            &s_recording,
            false
        );


        ESP_LOGI(
            TAG,
            "Recording buffer full"
        );


        return ESP_ERR_NO_MEM;
    }


    return ESP_OK;
}


esp_err_t voice_recorder_stop(
    voice_recording_t *recording
)
{
    if (
        recording == NULL ||
        s_samples == NULL
    ) {
        return ESP_ERR_INVALID_ARG;
    }


    atomic_store(
        &s_recording,
        false
    );


    const size_t sample_count =
        atomic_load(
            &s_sample_count
        );


    recording->samples =
        s_samples;

    recording->sample_count =
        sample_count;

    recording->sample_rate =
        VOICE_RECORDER_SAMPLE_RATE;


    const uint32_t duration_ms =
        (
            sample_count == 0U
        )
        ? 0U
        : (uint32_t)(
            (
                (uint64_t)sample_count *
                1000ULL
            ) /
            VOICE_RECORDER_SAMPLE_RATE
        );


    ESP_LOGI(
        TAG,
        "Recording stopped: %u samples, %u ms, %u bytes",
        (unsigned int)sample_count,
        (unsigned int)duration_ms,
        (unsigned int)(
            sample_count *
            sizeof(int16_t)
        )
    );


    return ESP_OK;
}


bool voice_recorder_is_recording(void)
{
    return atomic_load(
        &s_recording
    );
}


void voice_recorder_deinit(void)
{
    atomic_store(
        &s_recording,
        false
    );


    if (s_samples != NULL) {

        heap_caps_free(
            s_samples
        );

        s_samples = NULL;
    }


    s_capacity_samples = 0U;


    atomic_store(
        &s_sample_count,
        0U
    );
}
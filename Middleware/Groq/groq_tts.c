#include "groq_tts.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"

#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"

#include "audio_output.h"


#define GROQ_TTS_URL \
    "https://api.groq.com/openai/v1/audio/speech"

#define GROQ_TTS_MODEL \
    "canopylabs/orpheus-v1-english"

#define GROQ_TTS_VOICE \
    "autumn"

#define GROQ_TTS_SAMPLE_RATE 16000U

/*
 * Orpheus currently accepts at most 200 characters.
 * Keep a small margin for clean truncation.
 */
#define GROQ_TTS_INPUT_LIMIT 190U

#define GROQ_TTS_REQUEST_CAPACITY 1024U

/*
 * TTS response is stored completely in PSRAM before playback.
 *
 * 2 MiB is intentionally conservative:
 * mono PCM16 @ 16 kHz = 32000 bytes/s,
 * so 2 MiB can hold roughly 65 seconds of raw PCM.
 */
#define GROQ_TTS_MAX_RESPONSE_BYTES \
    (2U * 1024U * 1024U)

/*
 * Grow PSRAM buffer in 64 KiB steps.
 */
#define GROQ_TTS_GROW_BYTES \
    (64U * 1024U)


static const char *TAG = "GROQ_TTS";

static const char *s_api_key = NULL;


typedef struct {
    uint8_t *data;
    size_t size;
    size_t capacity;

    esp_err_t error;
} tts_download_ctx_t;


static uint16_t read_u16_le(
    const uint8_t *p
)
{
    return
        (uint16_t)p[0] |
        ((uint16_t)p[1] << 8);
}


static uint32_t read_u32_le(
    const uint8_t *p
)
{
    return
        (uint32_t)p[0] |
        ((uint32_t)p[1] << 8) |
        ((uint32_t)p[2] << 16) |
        ((uint32_t)p[3] << 24);
}


static void copy_tts_input(
    const char *source,
    char output[
        GROQ_TTS_INPUT_LIMIT + 1U
    ]
)
{
    size_t length =
        strlen(source);


    if (
        length <=
        GROQ_TTS_INPUT_LIMIT
    ) {

        memcpy(
            output,
            source,
            length + 1U
        );

        return;
    }


    size_t cut =
        GROQ_TTS_INPUT_LIMIT -
        3U;


    while (
        cut > 120U &&
        source[cut] != ' ' &&
        source[cut] != '\t' &&
        source[cut] != '\n'
    ) {
        cut--;
    }


    if (
        cut <= 120U
    ) {

        cut =
            GROQ_TTS_INPUT_LIMIT -
            3U;
    }


    memcpy(
        output,
        source,
        cut
    );


    output[cut++] = '.';
    output[cut++] = '.';
    output[cut++] = '.';
    output[cut] = '\0';
}


static esp_err_t ensure_download_capacity(
    tts_download_ctx_t *ctx,
    size_t required
)
{
    if (
        ctx == NULL
    ) {
        return ESP_ERR_INVALID_ARG;
    }


    if (
        required <=
        ctx->capacity
    ) {
        return ESP_OK;
    }


    if (
        required >
        GROQ_TTS_MAX_RESPONSE_BYTES
    ) {

        ESP_LOGE(
            TAG,
            "TTS response exceeds maximum: %u bytes",
            (unsigned int)required
        );

        return ESP_ERR_NO_MEM;
    }


    size_t new_capacity =
        ctx->capacity;


    if (
        new_capacity ==
        0U
    ) {

        new_capacity =
            GROQ_TTS_GROW_BYTES;
    }


    while (
        new_capacity <
        required
    ) {

        if (
            new_capacity >
            GROQ_TTS_MAX_RESPONSE_BYTES -
            GROQ_TTS_GROW_BYTES
        ) {

            new_capacity =
                GROQ_TTS_MAX_RESPONSE_BYTES;

        } else {

            new_capacity +=
                GROQ_TTS_GROW_BYTES;
        }


        if (
            new_capacity ==
            GROQ_TTS_MAX_RESPONSE_BYTES
        ) {
            break;
        }
    }


    uint8_t *new_buffer =
        heap_caps_realloc(
            ctx->data,
            new_capacity,
            MALLOC_CAP_SPIRAM |
            MALLOC_CAP_8BIT
        );


    if (
        new_buffer ==
        NULL
    ) {

        ESP_LOGE(
            TAG,
            "Cannot grow TTS PSRAM buffer to %u bytes",
            (unsigned int)new_capacity
        );

        return ESP_ERR_NO_MEM;
    }


    ctx->data =
        new_buffer;

    ctx->capacity =
        new_capacity;


    return ESP_OK;
}


static esp_err_t tts_http_event_handler(
    esp_http_client_event_t *event
)
{
    if (
        event == NULL ||
        event->user_data == NULL
    ) {
        return ESP_OK;
    }


    tts_download_ctx_t *ctx =
        (tts_download_ctx_t *)
        event->user_data;


    if (
        event->event_id !=
        HTTP_EVENT_ON_DATA ||
        event->data == NULL ||
        event->data_len <= 0
    ) {
        return ESP_OK;
    }


    if (
        ctx->error !=
        ESP_OK
    ) {
        return ctx->error;
    }


    const size_t incoming =
        (size_t)
        event->data_len;


    if (
        ctx->size >
        SIZE_MAX -
        incoming
    ) {

        ctx->error =
            ESP_ERR_INVALID_SIZE;

        return ctx->error;
    }


    const size_t required =
        ctx->size +
        incoming;


    ctx->error =
        ensure_download_capacity(
            ctx,
            required
        );


    if (
        ctx->error !=
        ESP_OK
    ) {
        return ctx->error;
    }


    memcpy(
        ctx->data +
            ctx->size,
        event->data,
        incoming
    );


    ctx->size =
        required;


    return ESP_OK;
}


static esp_err_t find_wav_pcm(
    const uint8_t *wav,
    size_t wav_size,
    const uint8_t **pcm_out,
    size_t *pcm_size_out
)
{
    if (
        wav == NULL ||
        pcm_out == NULL ||
        pcm_size_out == NULL
    ) {
        return ESP_ERR_INVALID_ARG;
    }


    if (
        wav_size <
        12U
    ) {
        return ESP_ERR_INVALID_RESPONSE;
    }


    if (
        memcmp(
            wav,
            "RIFF",
            4U
        ) != 0 ||
        memcmp(
            wav + 8U,
            "WAVE",
            4U
        ) != 0
    ) {

        ESP_LOGE(
            TAG,
            "TTS response is not RIFF/WAVE"
        );

        return ESP_ERR_INVALID_RESPONSE;
    }


    bool fmt_found =
        false;


    uint16_t audio_format =
        0U;

    uint16_t channels =
        0U;

    uint32_t sample_rate =
        0U;

    uint16_t bits_per_sample =
        0U;


    size_t position =
        12U;


    while (
        position + 8U <=
        wav_size
    ) {

        const uint8_t *chunk =
            wav + position;


        const uint32_t chunk_size_u32 =
            read_u32_le(
                chunk + 4U
            );


        const size_t chunk_size =
            (size_t)
            chunk_size_u32;


        const size_t data_offset =
            position +
            8U;


        if (
            data_offset >
            wav_size
        ) {
            return ESP_ERR_INVALID_RESPONSE;
        }


        /*
         * Streaming WAV encoders may leave a placeholder size in the
         * audio data chunk. Check for "data" before rejecting a chunk
         * whose declared size is larger than the downloaded body.
         */
        const bool is_data_chunk =
            memcmp(
                chunk,
                "data",
                4U
            ) == 0;


        if (
            !is_data_chunk &&
            chunk_size >
            wav_size -
            data_offset
        ) {

            ESP_LOGE(
                TAG,
                "Truncated WAV chunk %.4s: declared=%u available=%u",
                (const char *)chunk,
                (unsigned int)chunk_size,
                (unsigned int)(
                    wav_size -
                    data_offset
                )
            );

            return ESP_ERR_INVALID_RESPONSE;
        }


        if (
            memcmp(
                chunk,
                "fmt ",
                4U
            ) == 0
        ) {

            if (
                chunk_size <
                16U
            ) {

                ESP_LOGE(
                    TAG,
                    "Invalid WAV fmt chunk"
                );

                return ESP_ERR_INVALID_RESPONSE;
            }


            const uint8_t *fmt =
                wav +
                data_offset;


            audio_format =
                read_u16_le(
                    fmt + 0U
                );

            channels =
                read_u16_le(
                    fmt + 2U
                );

            sample_rate =
                read_u32_le(
                    fmt + 4U
                );

            bits_per_sample =
                read_u16_le(
                    fmt + 14U
                );


            fmt_found =
                true;


        } else if (
            memcmp(
                chunk,
                "data",
                4U
            ) == 0
        ) {

            if (
                !fmt_found
            ) {

                ESP_LOGE(
                    TAG,
                    "WAV data chunk before fmt chunk"
                );

                return ESP_ERR_INVALID_RESPONSE;
            }


            if (
                audio_format != 1U ||
                channels != 1U ||
                sample_rate !=
                    GROQ_TTS_SAMPLE_RATE ||
                bits_per_sample != 16U
            ) {

                ESP_LOGE(
                    TAG,
                    "Unsupported WAV: format=%u channels=%u rate=%u bits=%u",
                    (unsigned int)
                        audio_format,
                    (unsigned int)
                        channels,
                    (unsigned int)
                        sample_rate,
                    (unsigned int)
                        bits_per_sample
                );

                return ESP_ERR_NOT_SUPPORTED;
            }


            size_t pcm_size =
                chunk_size;


            const size_t available_pcm =
                wav_size -
                data_offset;


            /*
             * Groq can return a streaming-style WAV where the data
             * chunk size is a placeholder or simply larger than the
             * final HTTP body. In that case, use the bytes we actually
             * downloaded instead of rejecting the otherwise valid WAV.
             */
            if (
                pcm_size >
                available_pcm
            ) {

                ESP_LOGW(
                    TAG,
                    "WAV data size mismatch: declared=%u available=%u; using available bytes",
                    (unsigned int)
                        pcm_size,
                    (unsigned int)
                        available_pcm
                );


                pcm_size =
                    available_pcm;
            }


            /*
             * PCM16 needs complete 2-byte samples. Drop only one final
             * stray byte if the streamed WAV body ends on an odd byte.
             */
            if (
                (pcm_size %
                sizeof(int16_t)) != 0U
            ) {

                ESP_LOGW(
                    TAG,
                    "Odd PCM byte count: %u; dropping final byte",
                    (unsigned int)
                        pcm_size
                );


                pcm_size--;
            }


            if (
                pcm_size ==
                0U
            ) {

                ESP_LOGE(
                    TAG,
                    "WAV data chunk is empty"
                );

                return ESP_ERR_INVALID_RESPONSE;
            }


            *pcm_out =
                wav +
                data_offset;

            *pcm_size_out =
                pcm_size;


            ESP_LOGI(
                TAG,
                "WAV ready: mono PCM16 %u Hz, data=%u bytes",
                (unsigned int)
                    sample_rate,
                (unsigned int)
                    pcm_size
            );


            return ESP_OK;
        }


        size_t padded_size =
            chunk_size;


        if (
            padded_size &
            1U
        ) {
            padded_size++;
        }


        if (
            padded_size >
            SIZE_MAX -
            8U -
            position
        ) {

            return ESP_ERR_INVALID_SIZE;
        }


        position +=
            8U +
            padded_size;
    }


    ESP_LOGE(
        TAG,
        "WAV data chunk not found"
    );


    return ESP_ERR_INVALID_RESPONSE;
}


static esp_err_t play_wav_from_psram(
    const uint8_t *wav,
    size_t wav_size
)
{
    const uint8_t *pcm =
        NULL;

    size_t pcm_size =
        0U;


    esp_err_t error =
        find_wav_pcm(
            wav,
            wav_size,
            &pcm,
            &pcm_size
        );


    if (
        error !=
        ESP_OK
    ) {
        return error;
    }


    ESP_LOGI(
        TAG,
        "Starting TTS playback from PSRAM"
    );


    /*
     * audio_output already applies back-pressure through
     * its StreamBuffer, so it is safe to pass the full
     * PCM region here. Internally it feeds small blocks
     * to the I2S task.
     */
    error =
        audio_output_enqueue_pcm_mono16(
            pcm,
            pcm_size
        );


    if (
        error !=
        ESP_OK
    ) {

        ESP_LOGE(
            TAG,
            "TTS playback enqueue failed: %s",
            esp_err_to_name(error)
        );

        return error;
    }


    ESP_LOGI(
        TAG,
        "TTS PCM fully queued: %u bytes",
        (unsigned int)
            pcm_size
    );


    return ESP_OK;
}


esp_err_t groq_tts_init(
    const char *api_key
)
{
    if (
        api_key == NULL ||
        api_key[0] == '\0'
    ) {
        return ESP_ERR_INVALID_ARG;
    }


    s_api_key =
        api_key;


    ESP_LOGI(
        TAG,
        "Groq TTS initialized: model=%s voice=%s",
        GROQ_TTS_MODEL,
        GROQ_TTS_VOICE
    );


    return ESP_OK;
}


esp_err_t groq_tts_speak(
    const char *text
)
{
    if (
        s_api_key == NULL ||
        text == NULL ||
        text[0] == '\0'
    ) {
        return ESP_ERR_INVALID_ARG;
    }


    char tts_input[
        GROQ_TTS_INPUT_LIMIT + 1U
    ];


    copy_tts_input(
        text,
        tts_input
    );


    if (
        strcmp(
            tts_input,
            text
        ) != 0
    ) {

        ESP_LOGW(
            TAG,
            "TTS input truncated to %u characters",
            (unsigned int)
                strlen(tts_input)
        );
    }


    cJSON *root =
        cJSON_CreateObject();


    if (
        root == NULL
    ) {
        return ESP_ERR_NO_MEM;
    }


    cJSON_AddStringToObject(
        root,
        "model",
        GROQ_TTS_MODEL
    );


    cJSON_AddStringToObject(
        root,
        "input",
        tts_input
    );


    cJSON_AddStringToObject(
        root,
        "voice",
        GROQ_TTS_VOICE
    );


    cJSON_AddStringToObject(
        root,
        "response_format",
        "wav"
    );


    cJSON_AddNumberToObject(
        root,
        "sample_rate",
        GROQ_TTS_SAMPLE_RATE
    );


    char request_body[
        GROQ_TTS_REQUEST_CAPACITY
    ];


    bool json_ok =
        cJSON_PrintPreallocated(
            root,
            request_body,
            sizeof(request_body),
            false
        );


    cJSON_Delete(
        root
    );


    if (
        !json_ok
    ) {
        return ESP_ERR_INVALID_SIZE;
    }


    tts_download_ctx_t download = {
        .data =
            NULL,

        .size =
            0U,

        .capacity =
            0U,

        .error =
            ESP_OK,
    };


    esp_http_client_config_t config = {
        .url =
            GROQ_TTS_URL,

        .event_handler =
            tts_http_event_handler,

        .user_data =
            &download,

        .timeout_ms =
            60000,

        .crt_bundle_attach =
            esp_crt_bundle_attach,

        .buffer_size =
            4096,

        .buffer_size_tx =
            2048,

        .keep_alive_enable =
            false,
    };


    esp_http_client_handle_t client =
        esp_http_client_init(
            &config
        );


    if (
        client == NULL
    ) {
        return ESP_ERR_NO_MEM;
    }


    char authorization[192];


    int printed =
        snprintf(
            authorization,
            sizeof(authorization),
            "Bearer %s",
            s_api_key
        );


    if (
        printed <= 0 ||
        (size_t)printed >=
            sizeof(authorization)
    ) {

        esp_http_client_cleanup(
            client
        );

        return ESP_ERR_INVALID_SIZE;
    }


    esp_http_client_set_method(
        client,
        HTTP_METHOD_POST
    );


    esp_http_client_set_header(
        client,
        "Authorization",
        authorization
    );


    esp_http_client_set_header(
        client,
        "Content-Type",
        "application/json"
    );


    esp_err_t error =
        esp_http_client_set_post_field(
            client,
            request_body,
            (int)strlen(
                request_body
            )
        );


    if (
        error !=
        ESP_OK
    ) {

        esp_http_client_cleanup(
            client
        );

        return error;
    }


    ESP_LOGI(
        TAG,
        "Requesting speech: \"%s\"",
        tts_input
    );


    ESP_LOGI(
        TAG,
        "Downloading complete TTS WAV to PSRAM before playback"
    );


    error =
        esp_http_client_perform(
            client
        );


    const int status =
        esp_http_client_get_status_code(
            client
        );


    esp_http_client_cleanup(
        client
    );


    if (
        error !=
        ESP_OK
    ) {

        ESP_LOGE(
            TAG,
            "TTS HTTP request failed: %s",
            esp_err_to_name(error)
        );


        heap_caps_free(
            download.data
        );


        return error;
    }


    ESP_LOGI(
        TAG,
        "Groq TTS HTTP status: %d",
        status
    );


    if (
        download.error !=
        ESP_OK
    ) {

        ESP_LOGE(
            TAG,
            "TTS download failed: %s",
            esp_err_to_name(
                download.error
            )
        );


        heap_caps_free(
            download.data
        );


        return download.error;
    }


    if (
        status < 200 ||
        status >= 300
    ) {

        /*
         * For error responses Groq returns JSON rather
         * than WAV. Log a bounded printable preview.
         */
        if (
            download.data !=
            NULL &&
            download.size > 0U
        ) {

            const size_t preview_size =
                download.size < 400U
                    ? download.size
                    : 400U;


            char preview[401];


            memcpy(
                preview,
                download.data,
                preview_size
            );


            preview[preview_size] =
                '\0';


            ESP_LOGE(
                TAG,
                "Groq TTS response: %s",
                preview
            );
        }


        heap_caps_free(
            download.data
        );


        return ESP_FAIL;
    }


    ESP_LOGI(
        TAG,
        "TTS WAV downloaded to PSRAM: %u bytes (capacity=%u)",
        (unsigned int)
            download.size,
        (unsigned int)
            download.capacity
    );


    error =
        play_wav_from_psram(
            download.data,
            download.size
        );


    heap_caps_free(
        download.data
    );


    if (
        error !=
        ESP_OK
    ) {
        return error;
    }


    ESP_LOGI(
        TAG,
        "TTS playback finished"
    );


    return ESP_OK;
}
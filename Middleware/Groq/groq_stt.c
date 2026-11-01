#include "groq_stt.h"

#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"

#define GROQ_STT_URL \
    "https://api.groq.com/openai/v1/audio/transcriptions"

#define GROQ_STT_MODEL \
    "whisper-large-v3-turbo"

#define GROQ_STT_BOUNDARY \
    "----ESP32GroqBoundary7MA4YWxkTrZu0gW"

#define GROQ_STT_RESPONSE_CAPACITY (4U * 1024U)
#define GROQ_STT_UPLOAD_CHUNK_SIZE (8U * 1024U)

static const char *TAG = "GROQ_STT";
static const char *s_api_key = NULL;

static void put_u16_le(uint8_t *dst, uint16_t value)
{
    dst[0] = (uint8_t)(value & 0xFFU);
    dst[1] = (uint8_t)((value >> 8) & 0xFFU);
}

static void put_u32_le(uint8_t *dst, uint32_t value)
{
    dst[0] = (uint8_t)(value & 0xFFU);
    dst[1] = (uint8_t)((value >> 8) & 0xFFU);
    dst[2] = (uint8_t)((value >> 16) & 0xFFU);
    dst[3] = (uint8_t)((value >> 24) & 0xFFU);
}

static esp_err_t make_wav_header(
    uint8_t header[44],
    size_t pcm_bytes,
    uint32_t sample_rate
)
{
    if (
        header == NULL ||
        sample_rate == 0U ||
        pcm_bytes > UINT32_MAX - 36U ||
        sample_rate > UINT32_MAX / 2U
    ) {
        return ESP_ERR_INVALID_SIZE;
    }

    memcpy(&header[0], "RIFF", 4);
    put_u32_le(&header[4], (uint32_t)(36U + pcm_bytes));
    memcpy(&header[8], "WAVE", 4);
    memcpy(&header[12], "fmt ", 4);
    put_u32_le(&header[16], 16U);
    put_u16_le(&header[20], 1U);
    put_u16_le(&header[22], 1U);
    put_u32_le(&header[24], sample_rate);
    put_u32_le(&header[28], sample_rate * 2U);
    put_u16_le(&header[32], 2U);
    put_u16_le(&header[34], 16U);
    memcpy(&header[36], "data", 4);
    put_u32_le(&header[40], (uint32_t)pcm_bytes);

    return ESP_OK;
}

static esp_err_t http_write_all(
    esp_http_client_handle_t client,
    const uint8_t *data,
    size_t length
)
{
    if (client == NULL || data == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t offset = 0U;

    while (offset < length) {
        size_t remaining = length - offset;

        int request_size =
            (remaining > (size_t)INT_MAX)
            ? INT_MAX
            : (int)remaining;

        int written =
            esp_http_client_write(
                client,
                (const char *)(data + offset),
                request_size
            );

        if (written <= 0) {
            ESP_LOGE(
                TAG,
                "HTTP write failed at %u/%u bytes",
                (unsigned int)offset,
                (unsigned int)length
            );
            return ESP_FAIL;
        }

        offset += (size_t)written;
    }

    return ESP_OK;
}

esp_err_t groq_stt_init(const char *api_key)
{
    if (api_key == NULL || api_key[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    s_api_key = api_key;

    ESP_LOGI(TAG, "Groq STT client initialized");

    return ESP_OK;
}

esp_err_t groq_stt_transcribe_pcm16(
    const int16_t *samples,
    size_t sample_count,
    uint32_t sample_rate,
    char *text_buffer,
    size_t text_capacity
)
{
    if (
        s_api_key == NULL ||
        samples == NULL ||
        sample_count == 0U ||
        sample_rate == 0U ||
        text_buffer == NULL ||
        text_capacity == 0U
    ) {
        return ESP_ERR_INVALID_ARG;
    }

    text_buffer[0] = '\0';

    if (sample_count > SIZE_MAX / sizeof(int16_t)) {
        return ESP_ERR_INVALID_SIZE;
    }

    const size_t pcm_bytes =
        sample_count * sizeof(int16_t);

    static const char prefix[] =
        "--" GROQ_STT_BOUNDARY "\r\n"
        "Content-Disposition: form-data; name=\"model\"\r\n"
        "\r\n"
        GROQ_STT_MODEL "\r\n"
        "--" GROQ_STT_BOUNDARY "\r\n"
        "Content-Disposition: form-data; name=\"response_format\"\r\n"
        "\r\n"
        "json\r\n"
        "--" GROQ_STT_BOUNDARY "\r\n"
        "Content-Disposition: form-data; "
        "name=\"file\"; filename=\"recording.wav\"\r\n"
        "Content-Type: audio/wav\r\n"
        "\r\n";

    static const char suffix[] =
        "\r\n--" GROQ_STT_BOUNDARY "--\r\n";

    const size_t prefix_len = sizeof(prefix) - 1U;
    const size_t suffix_len = sizeof(suffix) - 1U;
    const size_t wav_header_len = 44U;

    if (
        prefix_len > SIZE_MAX - wav_header_len ||
        prefix_len + wav_header_len > SIZE_MAX - pcm_bytes ||
        prefix_len + wav_header_len + pcm_bytes >
            SIZE_MAX - suffix_len
    ) {
        return ESP_ERR_INVALID_SIZE;
    }

    const size_t body_len =
        prefix_len +
        wav_header_len +
        pcm_bytes +
        suffix_len;

    if (body_len > (size_t)INT_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t wav_header[44];

    esp_err_t error =
        make_wav_header(
            wav_header,
            pcm_bytes,
            sample_rate
        );

    if (error != ESP_OK) {
        return error;
    }

    char *response_buffer =
        heap_caps_calloc(
            1U,
            GROQ_STT_RESPONSE_CAPACITY,
            MALLOC_CAP_SPIRAM |
            MALLOC_CAP_8BIT
        );

    if (response_buffer == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_config_t config = {
        .url = GROQ_STT_URL,
        .timeout_ms = 60000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = 2048,
        .buffer_size_tx = 8192,
        .keep_alive_enable = false,
    };

    esp_http_client_handle_t client =
        esp_http_client_init(&config);

    if (client == NULL) {
        heap_caps_free(response_buffer);
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
        (size_t)printed >= sizeof(authorization)
    ) {
        esp_http_client_cleanup(client);
        heap_caps_free(response_buffer);
        return ESP_ERR_INVALID_SIZE;
    }

    char content_type[128];

    printed =
        snprintf(
            content_type,
            sizeof(content_type),
            "multipart/form-data; boundary=%s",
            GROQ_STT_BOUNDARY
        );

    if (
        printed <= 0 ||
        (size_t)printed >= sizeof(content_type)
    ) {
        esp_http_client_cleanup(client);
        heap_caps_free(response_buffer);
        return ESP_ERR_INVALID_SIZE;
    }

    error =
        esp_http_client_set_method(
            client,
            HTTP_METHOD_POST
        );

    if (error != ESP_OK) {
        goto cleanup;
    }

    error =
        esp_http_client_set_header(
            client,
            "Authorization",
            authorization
        );

    if (error != ESP_OK) {
        goto cleanup;
    }

    error =
        esp_http_client_set_header(
            client,
            "Content-Type",
            content_type
        );

    if (error != ESP_OK) {
        goto cleanup;
    }

    ESP_LOGI(
        TAG,
        "Opening STT upload: body=%u bytes, PCM=%u bytes",
        (unsigned int)body_len,
        (unsigned int)pcm_bytes
    );

    error =
        esp_http_client_open(
            client,
            (int)body_len
        );

    if (error != ESP_OK) {
        ESP_LOGE(
            TAG,
            "HTTP open failed: %s",
            esp_err_to_name(error)
        );
        goto cleanup;
    }

    error =
        http_write_all(
            client,
            (const uint8_t *)prefix,
            prefix_len
        );

    if (error != ESP_OK) {
        goto close_and_cleanup;
    }

    error =
        http_write_all(
            client,
            wav_header,
            sizeof(wav_header)
        );

    if (error != ESP_OK) {
        goto close_and_cleanup;
    }

    const uint8_t *pcm =
        (const uint8_t *)samples;

    size_t uploaded = 0U;
    size_t next_progress_log = 32U * 1024U;

    while (uploaded < pcm_bytes) {
        size_t chunk_size =
            pcm_bytes - uploaded;

        if (chunk_size > GROQ_STT_UPLOAD_CHUNK_SIZE) {
            chunk_size = GROQ_STT_UPLOAD_CHUNK_SIZE;
        }

        error =
            http_write_all(
                client,
                pcm + uploaded,
                chunk_size
            );

        if (error != ESP_OK) {
            ESP_LOGE(
                TAG,
                "PCM upload failed at %u/%u bytes",
                (unsigned int)uploaded,
                (unsigned int)pcm_bytes
            );
            goto close_and_cleanup;
        }

        uploaded += chunk_size;

        if (
            uploaded >= next_progress_log ||
            uploaded == pcm_bytes
        ) {
            ESP_LOGI(
                TAG,
                "STT upload progress: %u/%u bytes",
                (unsigned int)uploaded,
                (unsigned int)pcm_bytes
            );

            next_progress_log += 32U * 1024U;
        }

        taskYIELD();
    }

    error =
        http_write_all(
            client,
            (const uint8_t *)suffix,
            suffix_len
        );

    if (error != ESP_OK) {
        goto close_and_cleanup;
    }

    ESP_LOGI(
        TAG,
        "STT upload complete; waiting for response"
    );

    int64_t response_content_length =
        esp_http_client_fetch_headers(client);

    if (response_content_length < 0) {
        ESP_LOGE(
            TAG,
            "Failed to fetch response headers"
        );
        error = ESP_FAIL;
        goto close_and_cleanup;
    }

    const int status =
        esp_http_client_get_status_code(client);

    ESP_LOGI(
        TAG,
        "Groq STT HTTP status: %d",
        status
    );

    size_t response_length = 0U;

    while (
        response_length <
        GROQ_STT_RESPONSE_CAPACITY - 1U
    ) {
        int bytes_read =
            esp_http_client_read(
                client,
                response_buffer + response_length,
                (int)(
                    GROQ_STT_RESPONSE_CAPACITY -
                    1U -
                    response_length
                )
            );

        if (bytes_read < 0) {
            ESP_LOGE(
                TAG,
                "HTTP response read failed"
            );
            error = ESP_FAIL;
            goto close_and_cleanup;
        }

        if (bytes_read == 0) {
            break;
        }

        response_length +=
            (size_t)bytes_read;
    }

    response_buffer[response_length] = '\0';

    if (status < 200 || status >= 300) {
        ESP_LOGE(
            TAG,
            "Groq STT response: %.512s",
            response_buffer
        );
        error = ESP_FAIL;
        goto close_and_cleanup;
    }

    cJSON *root =
        cJSON_Parse(response_buffer);

    if (root == NULL) {
        ESP_LOGE(
            TAG,
            "Cannot parse Groq JSON: %.256s",
            response_buffer
        );
        error = ESP_ERR_INVALID_RESPONSE;
        goto close_and_cleanup;
    }

    const cJSON *text =
        cJSON_GetObjectItemCaseSensitive(
            root,
            "text"
        );

    if (
        !cJSON_IsString(text) ||
        text->valuestring == NULL
    ) {
        cJSON_Delete(root);
        error = ESP_ERR_INVALID_RESPONSE;
        goto close_and_cleanup;
    }

    snprintf(
        text_buffer,
        text_capacity,
        "%s",
        text->valuestring
    );

    cJSON_Delete(root);

    error = ESP_OK;

close_and_cleanup:
    esp_http_client_close(client);

cleanup:
    esp_http_client_cleanup(client);
    heap_caps_free(response_buffer);

    return error;
}
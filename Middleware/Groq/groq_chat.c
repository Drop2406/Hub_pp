#include "groq_chat.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"

#define GROQ_CHAT_URL \
    "https://api.groq.com/openai/v1/chat/completions"

#define GROQ_CHAT_MODEL \
    "openai/gpt-oss-20b"

#define GROQ_CHAT_REQUEST_CAPACITY  (4U * 1024U)
#define GROQ_CHAT_RESPONSE_CAPACITY (8U * 1024U)

static const char *TAG = "GROQ_CHAT";
static const char *s_api_key = NULL;

typedef struct {
    char *buffer;
    size_t capacity;
    size_t length;
} response_ctx_t;

static esp_err_t http_event_handler(
    esp_http_client_event_t *event
)
{
    if (
        event == NULL ||
        event->user_data == NULL ||
        event->event_id != HTTP_EVENT_ON_DATA ||
        event->data == NULL ||
        event->data_len <= 0
    ) {
        return ESP_OK;
    }

    response_ctx_t *ctx =
        (response_ctx_t *)event->user_data;

    if (ctx->length >= ctx->capacity - 1U) {
        return ESP_OK;
    }

    size_t available =
        ctx->capacity - ctx->length - 1U;

    size_t copy_size =
        (size_t)event->data_len;

    if (copy_size > available) {
        copy_size = available;
    }

    memcpy(
        ctx->buffer + ctx->length,
        event->data,
        copy_size
    );

    ctx->length += copy_size;
    ctx->buffer[ctx->length] = '\0';

    return ESP_OK;
}

esp_err_t groq_chat_init(
    const char *api_key
)
{
    if (
        api_key == NULL ||
        api_key[0] == '\0'
    ) {
        return ESP_ERR_INVALID_ARG;
    }

    s_api_key = api_key;

    ESP_LOGI(
        TAG,
        "Groq chat client initialized"
    );

    return ESP_OK;
}

esp_err_t groq_chat_ask(
    const char *user_text,
    char *answer_buffer,
    size_t answer_capacity
)
{
    if (
        s_api_key == NULL ||
        user_text == NULL ||
        user_text[0] == '\0' ||
        answer_buffer == NULL ||
        answer_capacity == 0U
    ) {
        return ESP_ERR_INVALID_ARG;
    }

    answer_buffer[0] = '\0';

    cJSON *root =
        cJSON_CreateObject();

    if (root == NULL) {
        return ESP_ERR_NO_MEM;
    }

    cJSON *messages =
        cJSON_AddArrayToObject(
            root,
            "messages"
        );

    if (messages == NULL) {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }

    cJSON *system_message =
        cJSON_CreateObject();

    cJSON *user_message =
        cJSON_CreateObject();

    if (
        system_message == NULL ||
        user_message == NULL
    ) {
        cJSON_Delete(system_message);
        cJSON_Delete(user_message);
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddStringToObject(
        system_message,
        "role",
        "system"
    );

    cJSON_AddStringToObject(
        system_message,
        "content",
        "You are a concise voice assistant. "
        "Answer naturally in one or two short sentences."
    );

    cJSON_AddStringToObject(
        user_message,
        "role",
        "user"
    );

    cJSON_AddStringToObject(
        user_message,
        "content",
        user_text
    );

    cJSON_AddItemToArray(
        messages,
        system_message
    );

    cJSON_AddItemToArray(
        messages,
        user_message
    );

    cJSON_AddStringToObject(
        root,
        "model",
        GROQ_CHAT_MODEL
    );

    cJSON_AddStringToObject(
        root,
        "reasoning_effort",
        "low"
    );

    cJSON_AddNumberToObject(
        root,
        "max_completion_tokens",
        128
    );

    char *request_buffer =
        heap_caps_malloc(
            GROQ_CHAT_REQUEST_CAPACITY,
            MALLOC_CAP_SPIRAM |
            MALLOC_CAP_8BIT
        );

    if (request_buffer == NULL) {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }

    bool printed =
        cJSON_PrintPreallocated(
            root,
            request_buffer,
            GROQ_CHAT_REQUEST_CAPACITY,
            false
        );

    cJSON_Delete(root);

    if (!printed) {
        heap_caps_free(request_buffer);
        ESP_LOGE(
            TAG,
            "Chat request JSON too large"
        );
        return ESP_ERR_INVALID_SIZE;
    }

    char *response_buffer =
        heap_caps_calloc(
            1U,
            GROQ_CHAT_RESPONSE_CAPACITY,
            MALLOC_CAP_SPIRAM |
            MALLOC_CAP_8BIT
        );

    if (response_buffer == NULL) {
        heap_caps_free(request_buffer);
        return ESP_ERR_NO_MEM;
    }

    response_ctx_t response = {
        .buffer = response_buffer,
        .capacity = GROQ_CHAT_RESPONSE_CAPACITY,
        .length = 0U,
    };

    esp_http_client_config_t config = {
        .url = GROQ_CHAT_URL,
        .event_handler = http_event_handler,
        .user_data = &response,
        .timeout_ms = 30000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = 2048,
        .buffer_size_tx = 2048,
        .keep_alive_enable = false,
    };

    esp_http_client_handle_t client =
        esp_http_client_init(
            &config
        );

    if (client == NULL) {
        heap_caps_free(response_buffer);
        heap_caps_free(request_buffer);
        return ESP_ERR_NO_MEM;
    }

    char authorization[192];

    int written =
        snprintf(
            authorization,
            sizeof(authorization),
            "Bearer %s",
            s_api_key
        );

    if (
        written <= 0 ||
        (size_t)written >=
            sizeof(authorization)
    ) {
        esp_http_client_cleanup(client);
        heap_caps_free(response_buffer);
        heap_caps_free(request_buffer);
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
            request_buffer,
            (int)strlen(request_buffer)
        );

    if (error != ESP_OK) {
        esp_http_client_cleanup(client);
        heap_caps_free(response_buffer);
        heap_caps_free(request_buffer);
        return error;
    }

    ESP_LOGI(
        TAG,
        "Sending transcript to %s",
        GROQ_CHAT_MODEL
    );

    error =
        esp_http_client_perform(
            client
        );

    if (error != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Chat HTTP request failed: %s",
            esp_err_to_name(error)
        );

        esp_http_client_cleanup(client);
        heap_caps_free(response_buffer);
        heap_caps_free(request_buffer);
        return error;
    }

    const int status =
        esp_http_client_get_status_code(
            client
        );

    ESP_LOGI(
        TAG,
        "Groq chat HTTP status: %d",
        status
    );

    esp_http_client_cleanup(client);
    heap_caps_free(request_buffer);

    if (
        status < 200 ||
        status >= 300
    ) {
        ESP_LOGE(
            TAG,
            "Groq chat response: %.512s",
            response_buffer
        );

        heap_caps_free(response_buffer);
        return ESP_FAIL;
    }

    cJSON *response_root =
        cJSON_Parse(
            response_buffer
        );

    if (response_root == NULL) {
        ESP_LOGE(
            TAG,
            "Cannot parse chat JSON: %.256s",
            response_buffer
        );

        heap_caps_free(response_buffer);
        return ESP_ERR_INVALID_RESPONSE;
    }

    const cJSON *choices =
        cJSON_GetObjectItemCaseSensitive(
            response_root,
            "choices"
        );

    const cJSON *choice0 =
        cJSON_IsArray(choices)
        ? cJSON_GetArrayItem(
            choices,
            0
        )
        : NULL;

    const cJSON *message =
        choice0 != NULL
        ? cJSON_GetObjectItemCaseSensitive(
            choice0,
            "message"
        )
        : NULL;

    const cJSON *content =
        message != NULL
        ? cJSON_GetObjectItemCaseSensitive(
            message,
            "content"
        )
        : NULL;

    if (
        !cJSON_IsString(content) ||
        content->valuestring == NULL
    ) {
        cJSON_Delete(response_root);
        heap_caps_free(response_buffer);
        return ESP_ERR_INVALID_RESPONSE;
    }

    snprintf(
        answer_buffer,
        answer_capacity,
        "%s",
        content->valuestring
    );

    cJSON_Delete(response_root);
    heap_caps_free(response_buffer);

    return ESP_OK;
}
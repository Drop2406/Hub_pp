#include "app_voice.h"

#include <stdbool.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "microphone.h"
#include "sd_card.h"

#include "app_media.h"
#include "app_led.h"

#include "model_path.h"
#include "esp_afe_config.h"
#include "esp_afe_sr_iface.h"
#include "esp_afe_sr_models.h"
#include "esp_mn_iface.h"
#include "esp_mn_models.h"
#include "esp_mn_speech_commands.h"


#define HELLO_AVI_PATH \
    SD_CARD_MOUNT_POINT "/ANIM/1HELLO~1.AVI"

#define HELLO_RETRIGGER_GUARD_MS 1500U
#define COMMAND_LISTEN_WINDOW_MS 8000U

#define COMMAND_LED_ON  1
#define COMMAND_LED_OFF 2

#define VOICE_TASK_STACK_BYTES (4U * 1024U)
#define STACK_LOG_PERIOD_MS     5000U


static const char *TAG = "APP_VOICE";


typedef enum {
    VOICE_WAIT_WAKE = 0,
    VOICE_PLAYING_HELLO,
    VOICE_LISTEN_COMMAND
} voice_state_t;


static TaskHandle_t s_hello_task_handle = NULL;

static atomic_int s_voice_state =
    ATOMIC_VAR_INIT(VOICE_WAIT_WAKE);


static const esp_afe_sr_iface_t *s_afe_handle = NULL;
static esp_afe_sr_data_t *s_afe_data = NULL;
static srmodel_list_t *s_sr_models = NULL;

static esp_mn_iface_t *s_mn_handle = NULL;
static model_iface_data_t *s_mn_data = NULL;


/*
 * Log FreeRTOS stack high-water mark.
 *
 * ESP-IDF reports this value in bytes on ESP32.
 */
static void log_stack_high_water(const char *task_name)
{
    UBaseType_t free_bytes =
        uxTaskGetStackHighWaterMark(NULL);

    ESP_LOGI(
        TAG,
        "[STACK] %s min_free=%u bytes",
        task_name,
        (unsigned int)free_bytes
    );
}


/*
 * Hello AVI task.
 */
static void hello_video_task(void *arg)
{
    (void)arg;

    while (1) {

        ulTaskNotifyTake(
            pdTRUE,
            portMAX_DELAY
        );

        ESP_LOGI(
            TAG,
            "Wake word -> play Hello AVI"
        );

        esp_err_t error =
            app_media_play(
                HELLO_AVI_PATH
            );

        if (error != ESP_OK) {

            ESP_LOGE(
                TAG,
                "Hello AVI failed: %s",
                esp_err_to_name(error)
            );
        }

        /*
         * Audio output is asynchronous.
         * Keep the original retrigger guard.
         */
        vTaskDelay(
            pdMS_TO_TICKS(
                HELLO_RETRIGGER_GUARD_MS
            )
        );

        atomic_store(
            &s_voice_state,
            VOICE_LISTEN_COMMAND
        );

        log_stack_high_water(
            "hello_video"
        );

        ESP_LOGI(
            TAG,
            "Hello finished -> waiting for Turn on / Turn off"
        );
    }
}


/*
 * MultiNet initialization.
 *
 * ID 1 -> Turn on
 * ID 2 -> Turn off
 */
static esp_err_t multinet_init(void)
{
    char *mn_name =
        esp_srmodel_filter(
            s_sr_models,
            ESP_MN_PREFIX,
            ESP_MN_ENGLISH
        );

    if (mn_name == NULL) {

        ESP_LOGE(
            TAG,
            "English MultiNet model not found in model partition"
        );

        return ESP_ERR_NOT_FOUND;
    }

    ESP_LOGI(
        TAG,
        "MultiNet model: %s",
        mn_name
    );

    s_mn_handle =
        esp_mn_handle_from_name(
            mn_name
        );

    if (s_mn_handle == NULL) {

        ESP_LOGE(
            TAG,
            "Cannot get MultiNet interface"
        );

        return ESP_FAIL;
    }

    s_mn_data =
        s_mn_handle->create(
            mn_name,
            COMMAND_LISTEN_WINDOW_MS
        );

    if (s_mn_data == NULL) {

        ESP_LOGE(
            TAG,
            "MultiNet create failed (check PSRAM/internal RAM)"
        );

        return ESP_ERR_NO_MEM;
    }

    const int afe_samples =
        s_afe_handle->get_fetch_chunksize(
            s_afe_data
        );

    const int mn_samples =
        s_mn_handle->get_samp_chunksize(
            s_mn_data
        );

    ESP_LOGI(
        TAG,
        "AFE fetch chunk=%d, MultiNet chunk=%d",
        afe_samples,
        mn_samples
    );

    if (
        afe_samples <= 0 ||
        afe_samples != mn_samples ||
        s_mn_handle->get_samp_rate(
            s_mn_data
        ) != 16000
    ) {

        ESP_LOGE(
            TAG,
            "AFE / MultiNet audio format mismatch"
        );

        return ESP_ERR_INVALID_SIZE;
    }

    esp_err_t error =
        esp_mn_commands_alloc(
            s_mn_handle,
            s_mn_data
        );

    if (error != ESP_OK) {

        ESP_LOGE(
            TAG,
            "MultiNet commands alloc failed: %s",
            esp_err_to_name(error)
        );

        return error;
    }

    error =
        esp_mn_commands_clear();

    if (error != ESP_OK) {
        return error;
    }

    error =
        esp_mn_commands_add(
            COMMAND_LED_ON,
            "turn on"
        );

    if (error != ESP_OK) {

        ESP_LOGE(
            TAG,
            "Cannot add 'turn on': %s",
            esp_err_to_name(error)
        );

        return error;
    }

    error =
        esp_mn_commands_add(
            COMMAND_LED_OFF,
            "turn off"
        );

    if (error != ESP_OK) {

        ESP_LOGE(
            TAG,
            "Cannot add 'turn off': %s",
            esp_err_to_name(error)
        );

        return error;
    }

    esp_mn_error_t *command_errors =
        esp_mn_commands_update();

    if (command_errors != NULL) {

        ESP_LOGE(
            TAG,
            "MultiNet rejected %d command phrase(s)",
            command_errors->num
        );

        return ESP_FAIL;
    }

    s_mn_handle->print_active_speech_commands(
        s_mn_data
    );

    ESP_LOGI(
        TAG,
        "MultiNet ready: Turn on / Turn off"
    );

    return ESP_OK;
}


/*
 * Return to WakeNet mode.
 * Only the fetch task calls this function.
 */
static void return_to_wake_mode(void)
{
    s_mn_handle->clean(
        s_mn_data
    );

    atomic_store(
        &s_voice_state,
        VOICE_WAIT_WAKE
    );

    int enabled =
        s_afe_handle->enable_wakenet(
            s_afe_data
        );

    if (enabled < 0) {

        ESP_LOGE(
            TAG,
            "Failed to re-enable WakeNet"
        );

    } else {

        ESP_LOGI(
            TAG,
            "Ready for next wake word"
        );
    }
}


/*
 * INMP441 -> AFE feed.
 */
static void wakenet_feed_task(void *arg)
{
    (void)arg;

    const int feed_samples =
        s_afe_handle->get_feed_chunksize(
            s_afe_data
        );

    const int feed_channels =
        s_afe_handle->get_feed_channel_num(
            s_afe_data
        );

    ESP_LOGI(
        TAG,
        "AFE feed: samples=%d channels=%d",
        feed_samples,
        feed_channels
    );

    if (feed_channels != 1) {

        ESP_LOGE(
            TAG,
            "Unexpected AFE channel count: %d",
            feed_channels
        );

        vTaskDelete(NULL);
        return;
    }

    int16_t *audio_buffer =
        malloc(
            (size_t)feed_samples *
            sizeof(int16_t)
        );

    if (audio_buffer == NULL) {

        ESP_LOGE(
            TAG,
            "Failed to allocate AFE feed buffer"
        );

        vTaskDelete(NULL);
        return;
    }

    int64_t next_stack_log_us =
        esp_timer_get_time() +
        (int64_t)STACK_LOG_PERIOD_MS * 1000;

    while (1) {

        size_t samples_read = 0U;

        esp_err_t error =
            microphone_read(
                audio_buffer,
                (size_t)feed_samples,
                &samples_read
            );

        if (error != ESP_OK) {

            ESP_LOGE(
                TAG,
                "Microphone read failed: %s",
                esp_err_to_name(error)
            );

            continue;
        }

        if (
            samples_read !=
            (size_t)feed_samples
        ) {

            ESP_LOGW(
                TAG,
                "Short microphone read: %u/%d",
                (unsigned int)samples_read,
                feed_samples
            );

            continue;
        }

        int result =
            s_afe_handle->feed(
                s_afe_data,
                audio_buffer
            );

        if (result < 0) {

            ESP_LOGW(
                TAG,
                "AFE feed failed: %d",
                result
            );
        }

        if (
            esp_timer_get_time() >=
            next_stack_log_us
        ) {

            log_stack_high_water(
                "wakenet_feed"
            );

            next_stack_log_us =
                esp_timer_get_time() +
                (int64_t)STACK_LOG_PERIOD_MS * 1000;
        }
    }
}


/*
 * AFE -> WakeNet / MultiNet.
 */
static void wakenet_fetch_task(void *arg)
{
    (void)arg;

    bool command_session_started =
        false;

    int64_t command_deadline_us =
        0;

    int64_t next_stack_log_us =
        esp_timer_get_time() +
        (int64_t)STACK_LOG_PERIOD_MS * 1000;

    ESP_LOGI(
        TAG,
        "WakeNet detector started; say: Hi ESP"
    );

    while (1) {

        afe_fetch_result_t *result =
            s_afe_handle->fetch(
                s_afe_data
            );

        if (
            result == NULL ||
            result->ret_value == ESP_FAIL
        ) {

            ESP_LOGE(
                TAG,
                "AFE fetch failed"
            );

            continue;
        }

        if (
            esp_timer_get_time() >=
            next_stack_log_us
        ) {

            log_stack_high_water(
                "wakenet_fetch"
            );

            next_stack_log_us =
                esp_timer_get_time() +
                (int64_t)STACK_LOG_PERIOD_MS * 1000;
        }

        voice_state_t state =
            (voice_state_t)
            atomic_load(
                &s_voice_state
            );

        /*
         * WAIT_WAKE
         */
        if (
            state ==
            VOICE_WAIT_WAKE
        ) {

            command_session_started =
                false;

            if (
                result->wakeup_state ==
                    WAKENET_DETECTED &&
                s_hello_task_handle != NULL
            ) {

                ESP_LOGI(
                    TAG,
                    "WAKE WORD DETECTED! model=%d word=%d",
                    result->wakenet_model_index,
                    result->wake_word_index
                );

                atomic_store(
                    &s_voice_state,
                    VOICE_PLAYING_HELLO
                );

                int disabled =
                    s_afe_handle->disable_wakenet(
                        s_afe_data
                    );

                if (disabled < 0) {

                    ESP_LOGW(
                        TAG,
                        "Cannot disable WakeNet during AVI"
                    );
                }

                xTaskNotifyGive(
                    s_hello_task_handle
                );
            }

            continue;
        }

        /*
         * PLAYING_HELLO
         *
         * Continue fetching only to drain AFE.
         */
        if (
            state ==
            VOICE_PLAYING_HELLO
        ) {
            continue;
        }

        /*
         * LISTEN_COMMAND
         */
        if (!command_session_started) {

            s_mn_handle->clean(
                s_mn_data
            );

            command_deadline_us =
                esp_timer_get_time() +
                (int64_t)
                COMMAND_LISTEN_WINDOW_MS *
                1000;

            command_session_started =
                true;

            ESP_LOGI(
                TAG,
                "Listening for Turn on / Turn off (%u ms)",
                (unsigned)
                COMMAND_LISTEN_WINDOW_MS
            );
        }

        if (
            esp_timer_get_time() >=
            command_deadline_us
        ) {

            ESP_LOGI(
                TAG,
                "Command window expired"
            );

            return_to_wake_mode();

            command_session_started =
                false;

            continue;
        }

        if (
            result->data == NULL ||
            result->data_size <
                (int)(
                    s_mn_handle->
                        get_samp_chunksize(
                            s_mn_data
                        ) *
                    sizeof(int16_t)
                )
        ) {
            continue;
        }

        esp_mn_state_t mn_state =
            s_mn_handle->detect(
                s_mn_data,
                result->data
            );

        if (
            mn_state ==
            ESP_MN_STATE_DETECTED
        ) {

            esp_mn_results_t *mn_result =
                s_mn_handle->get_results(
                    s_mn_data
                );

            if (
                mn_result != NULL &&
                mn_result->num > 0
            ) {

                const int command_id =
                    mn_result->command_id[0];

                ESP_LOGI(
                    TAG,
                    "MultiNet: id=%d phrase=%s confidence=%.3f",
                    command_id,
                    mn_result->string,
                    mn_result->prob[0]
                );

                if (
                    command_id ==
                    COMMAND_LED_ON
                ) {

                    app_led_set(true);

                    ESP_LOGI(
                        TAG,
                        "Turn on -> LED ON"
                    );

                } else if (
                    command_id ==
                    COMMAND_LED_OFF
                ) {

                    app_led_set(false);

                    ESP_LOGI(
                        TAG,
                        "Turn off -> LED OFF"
                    );

                } else {

                    ESP_LOGW(
                        TAG,
                        "Unknown command ID: %d",
                        command_id
                    );
                }

                s_mn_handle->clean(
                    s_mn_data
                );

                command_deadline_us =
                    esp_timer_get_time() +
                    (int64_t)
                    COMMAND_LISTEN_WINDOW_MS *
                    1000;

                ESP_LOGI(
                    TAG,
                    "Command processed; listening again (%u ms)",
                    (unsigned)
                    COMMAND_LISTEN_WINDOW_MS
                );
            }

        } else if (
            mn_state ==
            ESP_MN_STATE_TIMEOUT
        ) {

            ESP_LOGI(
                TAG,
                "MultiNet timeout"
            );

            return_to_wake_mode();

            command_session_started =
                false;
        }
    }
}


esp_err_t app_voice_init(void)
{
    ESP_LOGI(
        TAG,
        "============================"
    );

    ESP_LOGI(
        TAG,
        "ESP-SR WakeNet + MultiNet"
    );

    ESP_LOGI(
        TAG,
        "============================"
    );

    /*
     * 1. Microphone.
     */
    esp_err_t error =
        microphone_init();

    if (error != ESP_OK) {

        ESP_LOGE(
            TAG,
            "Microphone init failed: %s",
            esp_err_to_name(error)
        );

        return error;
    }

    /*
     * 2. Speech models.
     */
    s_sr_models =
        esp_srmodel_init(
            "model"
        );

    if (s_sr_models == NULL) {

        ESP_LOGE(
            TAG,
            "Failed to load ESP-SR models"
        );

        return ESP_FAIL;
    }

    ESP_LOGI(
        TAG,
        "Models loaded: %d",
        s_sr_models->num
    );

    for (
        int i = 0;
        i < s_sr_models->num;
        i++
    ) {

        ESP_LOGI(
            TAG,
            "Model[%d]: %s",
            i,
            s_sr_models->model_name[i]
        );
    }

    /*
     * "M" = one microphone.
     */
    afe_config_t *afe_config =
        afe_config_init(
            "M",
            s_sr_models,
            AFE_TYPE_SR,
            AFE_MODE_HIGH_PERF
        );

    if (afe_config == NULL) {

        ESP_LOGE(
            TAG,
            "afe_config_init failed"
        );

        return ESP_FAIL;
    }

    afe_config->aec_init =
        false;

    /*
     * Prefer PSRAM for AFE to preserve internal/DMA RAM.
     */
    afe_config->memory_alloc_mode =
        AFE_MEMORY_ALLOC_MORE_PSRAM;

    if (!afe_config->wakenet_init) {

        ESP_LOGE(
            TAG,
            "WakeNet is not enabled"
        );

        afe_config_free(
            afe_config
        );

        return ESP_FAIL;
    }

    ESP_LOGI(
        TAG,
        "WakeNet model: %s",
        afe_config->wakenet_model_name != NULL
            ? afe_config->wakenet_model_name
            : "(null)"
    );

    /*
     * 3. AFE interface.
     */
    s_afe_handle =
        esp_afe_handle_from_config(
            afe_config
        );

    if (s_afe_handle == NULL) {

        ESP_LOGE(
            TAG,
            "Failed to get AFE handle"
        );

        afe_config_free(
            afe_config
        );

        return ESP_FAIL;
    }

    /*
     * 4. AFE runtime.
     */
    s_afe_data =
        s_afe_handle->
            create_from_config(
                afe_config
            );

    if (s_afe_data == NULL) {

        ESP_LOGE(
            TAG,
            "Failed to create AFE"
        );

        afe_config_free(
            afe_config
        );

        return ESP_ERR_NO_MEM;
    }

    afe_config_free(
        afe_config
    );

    /*
     * 5. MultiNet.
     */
    error =
        multinet_init();

    if (error != ESP_OK) {

        ESP_LOGE(
            TAG,
            "MultiNet init failed: %s",
            esp_err_to_name(error)
        );

        return error;
    }

    s_afe_handle->print_pipeline(
        s_afe_data
    );

    ESP_LOGI(
        TAG,
        "AFE sample rate: %d Hz",
        s_afe_handle->get_samp_rate(
            s_afe_data
        )
    );

    ESP_LOGI(
        TAG,
        "AFE feed chunk: %d samples",
        s_afe_handle->get_feed_chunksize(
            s_afe_data
        )
    );

    /*
     * 6. Hello AVI task.
     */
    BaseType_t task_result =
        xTaskCreate(
            hello_video_task,
            "hello_video",
            VOICE_TASK_STACK_BYTES,
            NULL,
            4,
            &s_hello_task_handle
        );

    if (task_result != pdPASS) {

        ESP_LOGE(
            TAG,
            "Failed to create Hello video task"
        );

        return ESP_ERR_NO_MEM;
    }

    /*
     * 7. Feed task.
     */
    task_result =
        xTaskCreate(
            wakenet_feed_task,
            "wakenet_feed",
            VOICE_TASK_STACK_BYTES,
            NULL,
            5,
            NULL
        );

    if (task_result != pdPASS) {

        ESP_LOGE(
            TAG,
            "Failed to create feed task"
        );

        return ESP_ERR_NO_MEM;
    }

    /*
     * 8. Fetch task.
     */
    task_result =
        xTaskCreate(
            wakenet_fetch_task,
            "wakenet_fetch",
            VOICE_TASK_STACK_BYTES,
            NULL,
            5,
            NULL
        );

    if (task_result != pdPASS) {

        ESP_LOGE(
            TAG,
            "Failed to create fetch task"
        );

        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(
        TAG,
        "WakeNet + MultiNet ready"
    );

    return ESP_OK;
}
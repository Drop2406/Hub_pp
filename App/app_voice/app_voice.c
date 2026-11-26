#include "app_voice.h"

#include <stdbool.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "sdkconfig.h"

#include "microphone.h"
#include "sd_card.h"

#include "app_media.h"
#include "app_led.h"
#include "voice_recorder.h"
#include "groq_stt.h"
#include "groq_chat.h"
#include "groq_tts.h"

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
#define RECORD_DURATION_MS        5000U
#define STT_TASK_STACK_BYTES      (8U * 1024U)
#define STT_TASK_PRIORITY         5U
#define STT_QUEUE_LENGTH          1U
#define STT_TEXT_CAPACITY         512U
#define CHAT_ANSWER_CAPACITY      512U


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


typedef struct {
    int16_t *samples;
    size_t sample_count;
    uint32_t sample_rate;
} stt_job_t;


static QueueHandle_t s_stt_queue = NULL;
static bool s_stt_enabled = false;
static bool s_tts_enabled = false;
static atomic_bool s_tts_playing = ATOMIC_VAR_INIT(false);

static void log_stack_high_water(
    const char *task_name
);


static esp_err_t queue_recording_for_stt(
    const voice_recording_t *recording
)
{
    if (
        !s_stt_enabled ||
        s_stt_queue == NULL
    ) {
        return ESP_ERR_INVALID_STATE;
    }

    if (
        recording == NULL ||
        recording->samples == NULL ||
        recording->sample_count == 0U
    ) {
        return ESP_ERR_INVALID_ARG;
    }

    const size_t pcm_bytes =
        recording->sample_count *
        sizeof(int16_t);

    int16_t *copy =
        heap_caps_malloc(
            pcm_bytes,
            MALLOC_CAP_SPIRAM |
            MALLOC_CAP_8BIT
        );

    if (copy == NULL) {
        return ESP_ERR_NO_MEM;
    }

    memcpy(
        copy,
        recording->samples,
        pcm_bytes
    );

    stt_job_t job = {
        .samples = copy,
        .sample_count = recording->sample_count,
        .sample_rate = recording->sample_rate,
    };

    if (
        xQueueSend(
            s_stt_queue,
            &job,
            0
        ) != pdTRUE
    ) {
        heap_caps_free(copy);
        return ESP_ERR_TIMEOUT;
    }

    ESP_LOGI(
        TAG,
        "[STT] queued %u samples",
        (unsigned int)job.sample_count
    );

    return ESP_OK;
}


static void stt_task(void *arg)
{
    (void)arg;

    stt_job_t job;

    while (1) {

        if (
            xQueueReceive(
                s_stt_queue,
                &job,
                portMAX_DELAY
            ) != pdTRUE
        ) {
            continue;
        }

        char text[
            STT_TEXT_CAPACITY
        ];

        ESP_LOGI(
            TAG,
            "[STT] transcribing..."
        );

        esp_err_t error =
            groq_stt_transcribe_pcm16(
                job.samples,
                job.sample_count,
                job.sample_rate,
                text,
                sizeof(text)
            );

        if (error == ESP_OK) {

            ESP_LOGI(
                TAG,
                "[STT] text: %s",
                text
            );


            char answer[
                CHAT_ANSWER_CAPACITY
            ];


            ESP_LOGI(
                TAG,
                "[LLM] thinking..."
            );


            esp_err_t chat_error =
                groq_chat_ask(
                    text,
                    answer,
                    sizeof(answer)
                );


            if (
                chat_error ==
                ESP_OK
            ) {

                ESP_LOGI(
                    TAG,
                    "[LLM] answer: %s",
                    answer
                );


                if (
                    s_tts_enabled
                ) {

                    ESP_LOGI(
                        TAG,
                        "[TTS] speaking..."
                    );


                    atomic_store(
                        &s_tts_playing,
                        true
                    );


                    esp_err_t tts_error =
                        groq_tts_speak(
                            answer
                        );


                    atomic_store(
                        &s_tts_playing,
                        false
                    );


                    if (
                        tts_error ==
                        ESP_OK
                    ) {

                        ESP_LOGI(
                            TAG,
                            "[TTS] done"
                        );

                    } else {

                        ESP_LOGE(
                            TAG,
                            "[TTS] failed: %s",
                            esp_err_to_name(
                                tts_error
                            )
                        );
                    }
                }

            } else {

                ESP_LOGE(
                    TAG,
                    "[LLM] failed: %s",
                    esp_err_to_name(
                        chat_error
                    )
                );
            }

        } else {

            ESP_LOGE(
                TAG,
                "[STT] failed: %s",
                esp_err_to_name(error)
            );
        }

        heap_caps_free(
            job.samples
        );

        log_stack_high_water(
            "groq_stt"
        );
    }
}


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

        /*
         * The feed task remains the ONLY reader of the microphone.
         *
         * Recorder just taps the same PCM buffer and copies it to
         * PSRAM. This avoids two consumers competing for INMP441/I2S.
         */
        if (
            voice_recorder_is_recording()
        ) {

            esp_err_t recorder_error =
                voice_recorder_push(
                    audio_buffer,
                    samples_read
                );


            /*
             * ESP_ERR_NO_MEM here means the fixed-duration
             * PSRAM buffer has become full. This is expected.
             */
            if (
                recorder_error ==
                ESP_ERR_NO_MEM
            ) {

                voice_recording_t recording;


                if (
                    voice_recorder_stop(
                        &recording
                    ) ==
                    ESP_OK
                ) {

                    const uint32_t duration_ms =
                        (uint32_t)(
                            (
                                (uint64_t)
                                recording.sample_count *
                                1000ULL
                            ) /
                            recording.sample_rate
                        );


                    ESP_LOGI(
                        TAG,
                        "[REC] ready: %u samples, %u ms, %u bytes",
                        (unsigned int)
                            recording.sample_count,
                        (unsigned int)
                            duration_ms,
                        (unsigned int)(
                            recording.sample_count *
                            sizeof(int16_t)
                        )
                    );


                    if (s_stt_enabled) {

                        esp_err_t stt_error =
                            queue_recording_for_stt(
                                &recording
                            );

                        if (
                            stt_error !=
                            ESP_OK
                        ) {

                            ESP_LOGW(
                                TAG,
                                "Cannot queue STT job: %s",
                                esp_err_to_name(
                                    stt_error
                                )
                            );
                        }
                    }
                }

            } else if (
                recorder_error !=
                ESP_OK &&
                recorder_error !=
                ESP_ERR_INVALID_STATE
            ) {

                ESP_LOGW(
                    TAG,
                    "Recorder push failed: %s",
                    esp_err_to_name(
                        recorder_error
                    )
                );
            }
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
         * Speaker TTS can be picked up by the microphone.
         *
         * Keep draining AFE, but ignore WakeNet/MultiNet
         * decisions until TTS playback has finished.
         */
        if (
            atomic_load(
                &s_tts_playing
            )
        ) {

            command_session_started =
                false;

            continue;
        }

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


            esp_err_t recorder_error =
                voice_recorder_start();


            if (
                recorder_error ==
                ESP_OK
            ) {

                ESP_LOGI(
                    TAG,
                    "Recording user speech (%u ms max)",
                    (unsigned int)
                        RECORD_DURATION_MS
                );

            } else {

                ESP_LOGW(
                    TAG,
                    "Cannot start recorder: %s",
                    esp_err_to_name(
                        recorder_error
                    )
                );
            }


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
     * 2. Recorder.
     *
     * 5 seconds x 16000 samples/s x 2 bytes
     * = 160000 bytes in PSRAM.
     */
    error =
        voice_recorder_init(
            RECORD_DURATION_MS
        );


    if (error != ESP_OK) {

        ESP_LOGE(
            TAG,
            "Voice recorder init failed: %s",
            esp_err_to_name(error)
        );

        return error;
    }


    /*
     * 3. Groq STT.
     *
     * Optional: local voice functions continue even
     * when the key has not been configured.
     */
#if defined(CONFIG_GROQ_API_KEY)
    if (
        CONFIG_GROQ_API_KEY[0] != '\0'
    ) {

        error =
            groq_stt_init(
                CONFIG_GROQ_API_KEY
            );


        if (
            error ==
            ESP_OK
        ) {

            error =
                groq_chat_init(
                    CONFIG_GROQ_API_KEY
                );
        }


        if (
            error ==
            ESP_OK
        ) {

            esp_err_t tts_error =
                groq_tts_init(
                    CONFIG_GROQ_API_KEY
                );


            if (
                tts_error ==
                ESP_OK
            ) {

                s_tts_enabled =
                    true;

            } else {

                ESP_LOGW(
                    TAG,
                    "Groq TTS disabled: %s",
                    esp_err_to_name(
                        tts_error
                    )
                );
            }
        }


        if (error == ESP_OK) {

            s_stt_queue =
                xQueueCreate(
                    STT_QUEUE_LENGTH,
                    sizeof(stt_job_t)
                );

            if (s_stt_queue == NULL) {
                return ESP_ERR_NO_MEM;
            }

            BaseType_t stt_result =
                xTaskCreate(
                    stt_task,
                    "groq_stt",
                    STT_TASK_STACK_BYTES,
                    NULL,
                    STT_TASK_PRIORITY,
                    NULL
                );

            if (
                stt_result !=
                pdPASS
            ) {
                vQueueDelete(
                    s_stt_queue
                );

                s_stt_queue =
                    NULL;

                return ESP_ERR_NO_MEM;
            }

            s_stt_enabled =
                true;

            ESP_LOGI(
                TAG,
                "Groq STT ready"
            );

        } else {

            ESP_LOGW(
                TAG,
                "Groq STT disabled: %s",
                esp_err_to_name(error)
            );
        }

    } else {

        ESP_LOGW(
            TAG,
            "Groq API key is empty; STT disabled"
        );
    }
#endif


    /*
     * 4. Speech models.
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
     * 4. AFE interface.
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
     * 5. AFE runtime.
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
     * 6. MultiNet.
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
     * 7. Hello AVI task.
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
     * 8. Feed task.
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
     * 9. Fetch task.
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
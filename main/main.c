#include <stdbool.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "driver/gpio.h"

#include "avi_reader.h"
#include "image_decoder.h"
#include "sd_card.h"
#include "ssd1351.h"
#include "audio_output.h"
#include "microphone.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "wifi_sta.h"

#include <stdlib.h>

#include "model_path.h"
#include "esp_afe_config.h"
#include "esp_afe_sr_iface.h"
#include "esp_afe_sr_models.h"
#include "esp_mn_iface.h"
#include "esp_mn_models.h"
#include "esp_mn_speech_commands.h"

#define JPEG_BUFFER_CAPACITY (64U * 1024U)
#define RGB565_BUFFER_COUNT  2U
#define HELLO_AVI_PATH SD_CARD_MOUNT_POINT "/ANIM/1HELLO~1.AVI"
#define HELLO_RETRIGGER_GUARD_MS 1500U
#define COMMAND_LISTEN_WINDOW_MS 8000U
#define COMMAND_LED_ON  1
#define COMMAND_LED_OFF 2

#define LED_PIN GPIO_NUM_43

static const char *TAG = "MAIN";

static uint8_t *jpeg_buffer = NULL;

static uint8_t *rgb565_buffers[RGB565_BUFFER_COUNT] = {
    NULL,
    NULL
};

_Static_assert(
    JPEG_RGB565_BUFFER_SIZE == SSD1351_FRAME_BYTES,
    "JPEG output size does not match SSD1351 frame size"
);

static void print_internal_ram(const char *stage)
{
    ESP_LOGI(TAG,
             "[RAM] %s: free=%u, largest=%u bytes",
             stage,
             (unsigned)heap_caps_get_free_size(
                 MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(
                 MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
}

static EventGroupHandle_t s_wifi_events = NULL;

static void start_wifi(void)
{
    esp_err_t err;

    // 1. Khoi tao bo nho NVS.
    err = nvs_flash_init();
    if (err != ESP_OK) {
        goto fail;
    }

    // 2. Khoi tao TCP/IP stack.
    err = esp_netif_init();
    if (err != ESP_OK) {
        goto fail;
    }

    // 3. Tao default event loop.
    err = esp_event_loop_create_default();

    if (err != ESP_OK &&
        err != ESP_ERR_INVALID_STATE) {
        goto fail;
    }

    // 4. Tao Event Group de luu trang thai Wi-Fi.
    s_wifi_events = xEventGroupCreate();

    if (s_wifi_events == NULL) {
        err = ESP_ERR_NO_MEM;
        goto fail;
    }

    //print_internal_ram("BEFORE WIFI");

    // 5. Goi component Wi-Fi cua ban.
    err = wifi_sta_init(s_wifi_events);

    if (err != ESP_OK) {
        goto fail;
    }

    ESP_LOGI(TAG, "Wi-Fi initialized; waiting for IP");
    return;

fail:
    ESP_LOGE(TAG,
             "Wi-Fi startup failed: %s",
             esp_err_to_name(err));
}

static esp_err_t allocate_dma_framebuffers(void)
{
    for (size_t i = 0; i < RGB565_BUFFER_COUNT; i++) {

        rgb565_buffers[i] = heap_caps_malloc(
            JPEG_RGB565_BUFFER_SIZE,
            MALLOC_CAP_DMA |
            MALLOC_CAP_INTERNAL |
            MALLOC_CAP_8BIT
        );

        if (rgb565_buffers[i] == NULL) {

            ESP_LOGE(
                TAG,
                "Cannot allocate framebuffer %u",
                (unsigned int)i
            );

            return ESP_ERR_NO_MEM;
        }
    }

    ESP_LOGI(
        TAG,
        "Two DMA framebuffers allocated: %u bytes",
        (unsigned int)(
            JPEG_RGB565_BUFFER_SIZE *
            RGB565_BUFFER_COUNT
        )
    );

    return ESP_OK;
}

/*
 * Phát một file AVI từ đầu đến cuối.
 *
 * VIDEO:
 * JPEG -> RGB565 -> SSD1351
 *
 * AUDIO:
 * PCM mono 16-bit -> I2S -> MAX98357A
 */
static esp_err_t play_avi(
    const char *avi_path
)
{
    ESP_LOGI(
        TAG,
        "================================"
    );

    ESP_LOGI(
        TAG,
        "Playing AVI: %s",
        avi_path
    );

    /*
     * 1. Tìm LIST movi.
     */
    long movi_offset = 0;
    uint32_t movi_size = 0;

    esp_err_t error = avi_find_movi(
        avi_path,
        &movi_offset,
        &movi_size
    );

    if (error != ESP_OK) {

        ESP_LOGE(
            TAG,
            "Cannot find movi in %s: %s",
            avi_path,
            esp_err_to_name(error)
        );

        return error;
    }

    /*
     * 2. Mở stream AVI.
     */
    avi_stream_t avi_stream;

    error = avi_stream_open(
        &avi_stream,
        avi_path,
        movi_offset,
        movi_size
    );

    if (error != ESP_OK) {

        ESP_LOGE(
            TAG,
            "Cannot open AVI stream: %s",
            esp_err_to_name(error)
        );

        return error;
    }

    uint32_t frame_number = 0U;
    uint32_t decode_buffer_index = 0U;
    bool frame_in_flight = false;

    int64_t playback_start_us =
        esp_timer_get_time();

    while (1) {

        avi_chunk_type_t chunk_type;
        size_t chunk_size = 0U;

        /*
         * 3. Đọc media chunk tiếp theo.
         *
         * Có thể là VIDEO hoặc AUDIO.
         */
        error = avi_stream_read_next_chunk(
            &avi_stream,
            jpeg_buffer,
            JPEG_BUFFER_CAPACITY,
            &chunk_type,
            &chunk_size
        );

        /*
         * Hết AVI.
         */
        if (error == ESP_ERR_NOT_FOUND) {

            ESP_LOGI(
                TAG,
                "End of AVI: %s",
                avi_path
            );

            break;
        }

        if (error != ESP_OK) {

            ESP_LOGE(
                TAG,
                "Cannot read AVI chunk: %s",
                esp_err_to_name(error)
            );

            break;
        }

        /*
         * ==========================
         * AUDIO
         * ==========================
         *
         * AVI hiện tại:
         * PCM, mono, 16-bit, 16000 Hz.
         */
        if (chunk_type == AVI_CHUNK_AUDIO) {

            error = audio_output_enqueue_pcm_mono16(
                    jpeg_buffer,
                    chunk_size
                );

            if (error != ESP_OK) {

                ESP_LOGE(
                    TAG,
                    "Audio output failed: %s",
                    esp_err_to_name(error)
                );

                break;
            }

            /*
             * Xử lý xong audio chunk.
             * Đọc chunk tiếp theo.
             */
            continue;
        }

        /*
         * ==========================
         * VIDEO
         * ==========================
         */
        uint16_t decoded_width = 0U;
        uint16_t decoded_height = 0U;

        /*
         * 4. Decode JPEG vào framebuffer hiện không
         * được SPI DMA sử dụng.
         */
        error = jpeg_decoder_decode_rgb565(
            jpeg_buffer,
            chunk_size,
            rgb565_buffers[decode_buffer_index],
            JPEG_RGB565_BUFFER_SIZE,
            &decoded_width,
            &decoded_height
        );

        if (error != ESP_OK) {

            ESP_LOGE(
                TAG,
                "JPEG decode failed at frame %" PRIu32 ": %s",
                frame_number + 1U,
                esp_err_to_name(error)
            );

            break;
        }

        /*
         * 5. Chờ frame trước truyền xong.
         */
        if (frame_in_flight) {

            error = ssd1351_wait_frame_done();

            if (error != ESP_OK) {

                ESP_LOGE(
                    TAG,
                    "LCD DMA wait failed: %s",
                    esp_err_to_name(error)
                );

                break;
            }

            frame_in_flight = false;
        }

        /*
         * 6. Queue frame mới ra SSD1351.
         */
        error = ssd1351_queue_rgb565_frame(
            rgb565_buffers[decode_buffer_index]
        );

        if (error != ESP_OK) {

            ESP_LOGE(
                TAG,
                "Cannot queue frame %" PRIu32 ": %s",
                frame_number + 1U,
                esp_err_to_name(error)
            );

            break;
        }

        frame_in_flight = true;
        frame_number++;

        /*
         * Đổi framebuffer:
         * A -> B -> A -> B...
         */
        decode_buffer_index ^= 1U;

        if (
            frame_number <= 3U ||
            frame_number % 30U == 0U
        ) {

            ESP_LOGI(
                TAG,
                "Frame %" PRIu32
                " queued, JPEG=%u bytes",
                frame_number,
                (unsigned int)chunk_size
            );
        }
    }

    /*
     * 7. Chờ frame video cuối truyền xong.
     */
    if (frame_in_flight) {

        esp_err_t wait_error =
            ssd1351_wait_frame_done();

        if (wait_error != ESP_OK) {

            ESP_LOGE(
                TAG,
                "Final LCD DMA wait failed: %s",
                esp_err_to_name(wait_error)
            );

            error = wait_error;
        }
    }

    int64_t playback_end_us =
        esp_timer_get_time();

    /*
     * 8. Đóng file AVI.
     */
    avi_stream_close(
        &avi_stream
    );

    int64_t playback_time_us =
        playback_end_us -
        playback_start_us;

    ESP_LOGI(
        TAG,
        "AVI completed: %s",
        avi_path
    );

    ESP_LOGI(
        TAG,
        "Frames: %" PRIu32,
        frame_number
    );

    if (
        frame_number > 0U &&
        playback_time_us > 0
    ) {

        float fps =
            ((float)frame_number *
             1000000.0f) /
            (float)playback_time_us;

        ESP_LOGI(
            TAG,
            "Playback speed: %.2f FPS",
            fps
        );
    }

    return error == ESP_ERR_NOT_FOUND
        ? ESP_OK
        : error;
}

/*
 * WakeNet chỉ gửi thông báo; task riêng sẽ phát AVI.
 * Tránh chặn AFE fetch trong lúc đọc SD, giải mã JPEG và truyền SPI.
 */
/*
 * Only the fetch task controls WakeNet/MultiNet.
 * The video task only publishes the transition PLAYING -> LISTEN.
 */
typedef enum {
    VOICE_WAIT_WAKE = 0,
    VOICE_PLAYING_HELLO,
    VOICE_LISTEN_COMMAND
} voice_state_t;

static TaskHandle_t s_hello_task_handle = NULL;
static atomic_int s_voice_state = ATOMIC_VAR_INIT(VOICE_WAIT_WAKE);

static void hello_video_task(void *arg)
{
    (void)arg;

    while (1) {
        /* Đợi sự kiện đánh thức từ wakenet_fetch_task. */
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        ESP_LOGI(TAG, "Wake word -> play Hello AVI");

        esp_err_t error = play_avi(HELLO_AVI_PATH);
        if (error != ESP_OK) {
            ESP_LOGE(TAG, "Hello AVI failed: %s", esp_err_to_name(error));
        }

        /*
         * Audio output có Stream Buffer chạy bất đồng bộ: một ít âm thanh
         * có thể còn đang phát sau khi play_avi() trả về.
         * Đây là khoảng chờ thử nghiệm, chưa phải cơ chế chờ loa phát hết.
         */
        vTaskDelay(pdMS_TO_TICKS(HELLO_RETRIGGER_GUARD_MS));

        /*
         * After the video and audio guard, allow the fetch task
         * to start MultiNet. Do not call AFE control APIs here.
         */
        atomic_store(&s_voice_state, VOICE_LISTEN_COMMAND);
        ESP_LOGI(TAG, "Hello finished -> waiting for Turn on / Turn off");
    }
}

static const esp_afe_sr_iface_t *s_afe_handle = NULL;
static esp_afe_sr_data_t *s_afe_data = NULL;
static srmodel_list_t *s_sr_models = NULL;

static esp_mn_iface_t *s_mn_handle = NULL;
static model_iface_data_t *s_mn_data = NULL;

static esp_err_t board_led_init(void)
{
    /*
     * GPIO43 is active-low on the board you tested.
     * IMPORTANT: GPIO43 is also UART0 TX on ESP32-S3.
     * Use USB Serial/JTAG console if UART0 logs conflict with this LED.
     */
    esp_err_t error = gpio_reset_pin(LED_PIN);
    if (error != ESP_OK) {
        return error;
    }

    /* OFF before switching the pin into output mode. */
    error = gpio_set_level(LED_PIN, 1);
    if (error != ESP_OK) {
        return error;
    }

    error = gpio_set_direction(LED_PIN, GPIO_MODE_OUTPUT);
    if (error != ESP_OK) {
        return error;
    }

    return gpio_set_level(LED_PIN, 1);
}

static void board_led_set(bool enabled)
{
    /* Active-low: 0 = ON, 1 = OFF. */
    gpio_set_level(LED_PIN, enabled ? 0 : 1);
}

static esp_err_t multinet_init(void)
{
    /* Pick the English MultiNet model, not FST or WakeNet. */
    char *mn_name = esp_srmodel_filter(
        s_sr_models, ESP_MN_PREFIX, ESP_MN_ENGLISH
    );
    if (mn_name == NULL) {
        ESP_LOGE(TAG, "English MultiNet model not found in model partition");
        return ESP_ERR_NOT_FOUND;
    }

    ESP_LOGI(TAG, "MultiNet model: %s", mn_name);
    s_mn_handle = esp_mn_handle_from_name(mn_name);
    if (s_mn_handle == NULL) {
        ESP_LOGE(TAG, "Cannot get MultiNet interface");
        return ESP_FAIL;
    }

    s_mn_data = s_mn_handle->create(mn_name, COMMAND_LISTEN_WINDOW_MS);
    if (s_mn_data == NULL) {
        ESP_LOGE(TAG, "MultiNet create failed (check PSRAM/internal RAM)");
        return ESP_ERR_NO_MEM;
    }

    const int afe_samples = s_afe_handle->get_fetch_chunksize(s_afe_data);
    const int mn_samples = s_mn_handle->get_samp_chunksize(s_mn_data);
    ESP_LOGI(TAG, "AFE fetch chunk=%d, MultiNet chunk=%d",
             afe_samples, mn_samples);

    /* AFE fetch provides exactly one MultiNet frame per call. */
    if (afe_samples <= 0 || afe_samples != mn_samples ||
        s_mn_handle->get_samp_rate(s_mn_data) != 16000) {
        ESP_LOGE(TAG, "AFE / MultiNet audio format mismatch");
        return ESP_ERR_INVALID_SIZE;
    }

    /*
     * Initialize the command list explicitly before clear/add/update.
     * Calling clear() on a non-allocated list returns INVALID_STATE.
     */
    esp_err_t error = esp_mn_commands_alloc(s_mn_handle, s_mn_data);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "MultiNet commands alloc failed: %s",
                 esp_err_to_name(error));
        return error;
    }

    error = esp_mn_commands_clear();
    if (error != ESP_OK) {
        return error;
    }
    error = esp_mn_commands_add(COMMAND_LED_ON, "turn on");
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "Cannot add 'turn on': %s", esp_err_to_name(error));
        return error;
    }
    error = esp_mn_commands_add(COMMAND_LED_OFF, "turn off");
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "Cannot add 'turn off': %s", esp_err_to_name(error));
        return error;
    }

    esp_mn_error_t *command_errors = esp_mn_commands_update();
    if (command_errors != NULL) {
        ESP_LOGE(TAG, "MultiNet rejected %d command phrase(s)",
                 command_errors->num);
        return ESP_FAIL;
    }

    s_mn_handle->print_active_speech_commands(s_mn_data);
    ESP_LOGI(TAG, "MultiNet ready: Turn on / Turn off");
    return ESP_OK;
}

/* Called exclusively from the fetch task, never the video task. */
static void return_to_wake_mode(void)
{
    s_mn_handle->clean(s_mn_data);
    atomic_store(&s_voice_state, VOICE_WAIT_WAKE);

    int enabled = s_afe_handle->enable_wakenet(s_afe_data);
    if (enabled < 0) {
        ESP_LOGE(TAG, "Failed to re-enable WakeNet");
    } else {
        ESP_LOGI(TAG, "Ready for next wake word");
    }
}

static void wakenet_feed_task(void *arg)
{
    (void)arg;

    const int feed_samples =
        s_afe_handle->get_feed_chunksize(s_afe_data);

    const int feed_channels =
        s_afe_handle->get_feed_channel_num(s_afe_data);

    ESP_LOGI(TAG,
             "AFE feed: samples=%d channels=%d",
             feed_samples,
             feed_channels);

    /*
     * Ta khai báo AFE input là "M":
     * một microphone duy nhất.
     */
    if (feed_channels != 1) {
        ESP_LOGE(TAG,
                 "Unexpected AFE channel count: %d",
                 feed_channels);
        vTaskDelete(NULL);
        return;
    }

    int16_t *audio_buffer =
        malloc((size_t)feed_samples * sizeof(int16_t));

    if (audio_buffer == NULL) {
        ESP_LOGE(TAG, "Failed to allocate AFE feed buffer");
        vTaskDelete(NULL);
        return;
    }

    while (1) {
        size_t samples_read = 0U;

        esp_err_t error =
            microphone_read(
                audio_buffer,
                (size_t)feed_samples,
                &samples_read
            );

        if (error != ESP_OK) {
            ESP_LOGE(TAG,
                     "Microphone read failed: %s",
                     esp_err_to_name(error));
            continue;
        }

        if (samples_read != (size_t)feed_samples) {
            ESP_LOGW(TAG,
                     "Short microphone read: %u/%d",
                     (unsigned int)samples_read,
                     feed_samples);
            continue;
        }

        int result =
            s_afe_handle->feed(
                s_afe_data,
                audio_buffer
            );

        if (result < 0) {
            ESP_LOGW(TAG, "AFE feed failed: %d", result);
        }
    }
}


static void wakenet_fetch_task(void *arg)
{
    (void)arg;

    bool command_session_started = false;
    int64_t command_deadline_us = 0;

    ESP_LOGI(TAG, "WakeNet detector started; say: Hi ESP");

    while (1) {
        /*
         * Always fetch, including during AVI playback, so the AFE
         * ring buffer does not fill while the video is playing.
         */
        afe_fetch_result_t *result = s_afe_handle->fetch(s_afe_data);
        if (result == NULL || result->ret_value == ESP_FAIL) {
            ESP_LOGE(TAG, "AFE fetch failed");
            continue;
        }

        voice_state_t state = (voice_state_t)atomic_load(&s_voice_state);

        if (state == VOICE_WAIT_WAKE) {
            command_session_started = false;

            if (result->wakeup_state == WAKENET_DETECTED &&
                s_hello_task_handle != NULL) {
                ESP_LOGI(TAG, "WAKE WORD DETECTED! model=%d word=%d",
                         result->wakenet_model_index,
                         result->wake_word_index);

                /* Ignore wake events while video/command session is active. */
                atomic_store(&s_voice_state, VOICE_PLAYING_HELLO);

                int disabled = s_afe_handle->disable_wakenet(s_afe_data);
                if (disabled < 0) {
                    ESP_LOGW(TAG, "Cannot disable WakeNet during AVI");
                }

                xTaskNotifyGive(s_hello_task_handle);
            }
            continue;
        }

        if (state == VOICE_PLAYING_HELLO) {
            /* Keep draining AFE; never recognize video audio as a command. */
            continue;
        }

        /* The video task changes the shared state after the guard period. */
        if (!command_session_started) {
            s_mn_handle->clean(s_mn_data);
            command_deadline_us = esp_timer_get_time() +
                                  (int64_t)COMMAND_LISTEN_WINDOW_MS * 1000;
            command_session_started = true;
            ESP_LOGI(TAG, "Listening for Turn on / Turn off (%u ms)",
                     (unsigned)COMMAND_LISTEN_WINDOW_MS);
        }

        if (esp_timer_get_time() >= command_deadline_us) {
            ESP_LOGI(TAG, "Command window expired");
            return_to_wake_mode();
            command_session_started = false;
            continue;
        }

        if (result->data == NULL ||
            result->data_size < (int)(s_mn_handle->get_samp_chunksize(s_mn_data)
                                      * sizeof(int16_t))) {
            continue;
        }

        esp_mn_state_t mn_state = s_mn_handle->detect(
            s_mn_data, result->data
        );

        if (mn_state == ESP_MN_STATE_DETECTED) {
            esp_mn_results_t *mn_result = s_mn_handle->get_results(s_mn_data);
            if (mn_result != NULL && mn_result->num > 0) {
                const int command_id = mn_result->command_id[0];
                ESP_LOGI(TAG, "MultiNet: id=%d phrase=%s confidence=%.3f",
                         command_id, mn_result->string, mn_result->prob[0]);

                if (command_id == COMMAND_LED_ON) {
                    board_led_set(true);
                    ESP_LOGI(TAG, "Turn on -> LED ON");
                } else if (command_id == COMMAND_LED_OFF) {
                    board_led_set(false);
                    ESP_LOGI(TAG, "Turn off -> LED OFF");
                } else {
                    ESP_LOGW(TAG, "Unknown command ID: %d", command_id);
                }

                /*
                 * Keep listening after a command instead of returning to
                 * WakeNet. Reset MultiNet's recognition state so the next
                 * utterance can be detected, and restart the inactivity
                 * timer from this command.
                 */
                s_mn_handle->clean(s_mn_data);
                command_deadline_us = esp_timer_get_time() +
                                      (int64_t)COMMAND_LISTEN_WINDOW_MS * 1000;
                ESP_LOGI(TAG, "Command processed; listening again (%u ms)",
                         (unsigned)COMMAND_LISTEN_WINDOW_MS);
            }
        } else if (mn_state == ESP_MN_STATE_TIMEOUT) {
            ESP_LOGI(TAG, "MultiNet timeout");
            return_to_wake_mode();
            command_session_started = false;
        }
    }
}

void app_main(void)
{
    /*
     * 1. Khôi phục các thành phần của pipeline AVI đã kiểm tra trước đây.
     * Không phát AVI tại boot: chỉ phát khi WakeNet báo phát hiện.
     */
    ESP_ERROR_CHECK(ssd1351_init());
    jpeg_buffer = heap_caps_malloc(
        JPEG_BUFFER_CAPACITY,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
    );

    if (jpeg_buffer == NULL) {
        ESP_LOGE(TAG, "Cannot allocate JPEG buffer in PSRAM");
        return;
    }

    ESP_LOGI(TAG, "JPEG buffer allocated in PSRAM");
    ESP_ERROR_CHECK(allocate_dma_framebuffers());
    ESP_ERROR_CHECK(audio_output_init());
    ESP_ERROR_CHECK(ssd1351_fill_screen(SSD1351_RGB565(0, 0, 0)));
    ESP_ERROR_CHECK(sd_card_mount());
    print_internal_ram("BOOT");
    print_internal_ram("BEFORE WIFI");

    start_wifi();

    print_internal_ram("AFTER WIFI");
    /* Phát video trong task riêng, không chặn nhiệm vụ nhận diện. */
    BaseType_t task_result = xTaskCreate(
        hello_video_task,
        "hello_video",
        8 * 1024,
        NULL,
        4,
        &s_hello_task_handle
    );
    if (task_result != pdPASS) {
        ESP_LOGE(TAG, "Failed to create Hello video task");
        return;
    }

    /*
     * 2. Khởi động WakeNet như bản đã chạy thành công.
     */
    ESP_LOGI(TAG, "============================");
    ESP_LOGI(TAG, "ESP-SR WakeNet + MultiNet test");
    ESP_LOGI(TAG, "============================");

    /*
     * 1. Khởi tạo INMP441.
     */
    ESP_ERROR_CHECK(microphone_init());

    /*
     * 2. Load model từ partition có label "model".
     */
    s_sr_models = esp_srmodel_init("model");

    if (s_sr_models == NULL) {
        ESP_LOGE(TAG, "Failed to load ESP-SR models");
        return;
    }

    ESP_LOGI(TAG,
             "Models loaded: %d",
             s_sr_models->num);

    for (int i = 0; i < s_sr_models->num; i++) {
        ESP_LOGI(TAG,
                 "Model[%d]: %s",
                 i,
                 s_sr_models->model_name[i]);
    }

    /*
     * "M" = một microphone channel.
     *
     * Không có R (playback reference) vì bài test
     * này chưa sử dụng speaker/AEC.
     */
    afe_config_t *afe_config =
        afe_config_init(
            "M",
            s_sr_models,
            AFE_TYPE_SR,
            AFE_MODE_HIGH_PERF
        );

    if (afe_config == NULL) {
        ESP_LOGE(TAG, "afe_config_init failed");
        return;
    }

    /*
     * Không có playback-reference channel,
     * nên không dùng echo cancellation.
     */
    afe_config->aec_init = false;

    /* Ưu tiên PSRAM cho AFE để dành internal/DMA RAM cho AVI và I2S. */
    afe_config->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;

    if (!afe_config->wakenet_init) {
        ESP_LOGE(TAG, "WakeNet is not enabled");
        afe_config_free(afe_config);
        return;
    }

    ESP_LOGI(TAG,
             "WakeNet model: %s",
             afe_config->wakenet_model_name != NULL
                 ? afe_config->wakenet_model_name
                 : "(null)");

    /*
     * 3. Tạo AFE interface.
     */
    s_afe_handle =
        esp_afe_handle_from_config(afe_config);

    if (s_afe_handle == NULL) {
        ESP_LOGE(TAG, "Failed to get AFE handle");
        afe_config_free(afe_config);
        return;
    }

    /*
     * 4. Tạo AFE runtime instance.
     */
    s_afe_data =
        s_afe_handle->create_from_config(afe_config);

    if (s_afe_data == NULL) {
        ESP_LOGE(TAG, "Failed to create AFE");
        afe_config_free(afe_config);
        return;
    }

    afe_config_free(afe_config);

    /* Load MultiNet before starting the feed/fetch tasks. */
    esp_err_t mn_error = multinet_init();
    if (mn_error != ESP_OK) {
        ESP_LOGE(TAG, "MultiNet init failed: %s", esp_err_to_name(mn_error));
        return;
    }

    /*
     * GPIO43 is shared with UART0 TX. For a stable LED and logs,
     * route console to USB Serial/JTAG if it currently uses UART0.
     */
    ESP_ERROR_CHECK(board_led_init());

    /*
     * In pipeline để debug.
     */
    s_afe_handle->print_pipeline(s_afe_data);

    ESP_LOGI(
        TAG,
        "AFE sample rate: %d Hz",
        s_afe_handle->get_samp_rate(s_afe_data)
    );

    ESP_LOGI(
        TAG,
        "AFE feed chunk: %d samples",
        s_afe_handle->get_feed_chunksize(s_afe_data)
    );

    /*
     * feed task:
     * INMP441 -> AFE
     */
    task_result =
        xTaskCreate(
            wakenet_feed_task,
            "wakenet_feed",
            6 * 1024,
            NULL,
            5,
            NULL
        );

    if (task_result != pdPASS) {
        ESP_LOGE(TAG, "Failed to create feed task");
        return;
    }

    /*
     * fetch task:
     * AFE -> WakeNet detection result
     */
    task_result =
        xTaskCreate(
            wakenet_fetch_task,
            "wakenet_fetch",
            6 * 1024,
            NULL,
            5,
            NULL
        );

    if (task_result != pdPASS) {
        ESP_LOGE(TAG, "Failed to create fetch task");
        return;
    }

    ESP_LOGI(TAG, "WakeNet + MultiNet ready");
    print_internal_ram("HUB READY");

    while (1) {

        vTaskDelay(
            pdMS_TO_TICKS(1000)
        );
    }
}
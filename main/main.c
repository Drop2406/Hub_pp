#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"

#include "avi_reader.h"
#include "image_decoder.h"
#include "sd_card.h"
#include "ssd1351.h"
#include "audio_output.h"

#define JPEG_BUFFER_CAPACITY (64U * 1024U)
#define RGB565_BUFFER_COUNT  2U

static const char *TAG = "MAIN";

static uint8_t jpeg_buffer[JPEG_BUFFER_CAPACITY];

static uint8_t *rgb565_buffers[RGB565_BUFFER_COUNT] = {
    NULL,
    NULL
};

_Static_assert(
    JPEG_RGB565_BUFFER_SIZE == SSD1351_FRAME_BYTES,
    "JPEG output size does not match SSD1351 frame size"
);

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
            sizeof(jpeg_buffer),
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

            error = audio_output_write_pcm_mono16(
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


void app_main(void)
{
    ESP_LOGI(
        TAG,
        "Starting multi-AVI playback"
    );

    /*
     * 1. Khởi tạo SSD1351.
     */
    esp_err_t error =
        ssd1351_init();

    if (error != ESP_OK) {

        ESP_LOGE(
            TAG,
            "SSD1351 init failed: %s",
            esp_err_to_name(error)
        );

        return;
    }

    /*
     * 2. Cấp phát 2 DMA framebuffer.
     */
    error =
        allocate_dma_framebuffers();

    if (error != ESP_OK) {
        return;
    }

    /*
     * 3. Khởi tạo I2S cho MAX98357A.
     */
    error =
        audio_output_init();

    if (error != ESP_OK) {

        ESP_LOGE(
            TAG,
            "Audio init failed: %s",
            esp_err_to_name(error)
        );

        return;
    }

    /*
     * 4. Clear màn hình.
     */
    error = ssd1351_fill_screen(
        SSD1351_RGB565(0, 0, 0)
    );

    if (error != ESP_OK) {

        ESP_LOGE(
            TAG,
            "Cannot clear SSD1351: %s",
            esp_err_to_name(error)
        );

        return;
    }

    /*
     * 5. Mount SD.
     */
    error =
        sd_card_mount();

    if (error != ESP_OK) {

        ESP_LOGE(
            TAG,
            "SD card initialization failed: %s",
            esp_err_to_name(error)
        );

        return;
    }

    /*
     * 6. Danh sách video.
     */
    static const char *video_list[] = {

        SD_CARD_MOUNT_POINT
        "/ANIM/1HELLO~1.AVI",

        SD_CARD_MOUNT_POINT
        "/ANIM/25SUPR~1.AVI",

        SD_CARD_MOUNT_POINT
        "/ANIM/3ANGRY~1.AVI",
    };

    const size_t video_count =
        sizeof(video_list) /
        sizeof(video_list[0]);


    uint32_t frame_interval_us = 0U;


    error = avi_get_frame_interval(
        video_list[0],
        &frame_interval_us
    );


    if (
        error == ESP_OK &&
        frame_interval_us > 0U
    ) {

        float source_fps =
            1000000.0f /
            (float)frame_interval_us;


        ESP_LOGI(
            TAG,
            "AVI frame interval: %u us",
            (unsigned int)frame_interval_us
        );


        ESP_LOGI(
            TAG,
            "AVI source FPS: %.2f",
            source_fps
        );
    }
    /*
     * 7. Phát lần lượt các video.
     */
    for (
        size_t video_index = 0;
        video_index < video_count;
        video_index++
    ) {

        ESP_LOGI(
            TAG,
            "Starting video %u / %u",
            (unsigned int)(video_index + 1U),
            (unsigned int)video_count
        );

        error = play_avi(
            video_list[video_index]
        );

        if (error != ESP_OK) {

            ESP_LOGE(
                TAG,
                "Video %u failed: %s",
                (unsigned int)(video_index + 1U),
                esp_err_to_name(error)
            );

            /*
             * Nếu video này lỗi thì vẫn thử video kế tiếp.
             */
        }
    }

    ESP_LOGI(
        TAG,
        "All videos completed"
    );

    /*
     * 8. Giữ frame cuối cùng.
     */
    while (1) {

        vTaskDelay(
            pdMS_TO_TICKS(1000)
        );
    }
}
#include "audio_output.h"

#include <stddef.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/stream_buffer.h"

#include "driver/gpio.h"
#include "driver/i2s_std.h"

#include "esp_log.h"


#define AUDIO_PIN_LRC   GPIO_NUM_4
#define AUDIO_PIN_BCLK  GPIO_NUM_5
#define AUDIO_PIN_DOUT  GPIO_NUM_6

#define AUDIO_SAMPLE_RATE          16000U
#define TEST_TONE_FREQUENCY        440U
#define TEST_TONE_DURATION_MS      1000U

/*
 * PCM trong AVI hiện tại là:
 *
 * - 16-bit
 * - mono
 * - little-endian
 * - 16000 Hz
 *
 * 16000 sample/s * 2 byte/sample = 32000 byte/s.
 *
 * Buffer 8 KB tương đương khoảng 0.256 giây audio.
 */
#define AUDIO_STREAM_BUFFER_SIZE   (8U * 1024U)

/*
 * Mỗi lần audio task xử lý tối đa 256 mono sample.
 *
 * 256 sample * 2 byte = 512 byte input.
 */
#define AUDIO_MONO_BLOCK_SAMPLES   256U
#define AUDIO_MONO_BLOCK_BYTES \
    (AUDIO_MONO_BLOCK_SAMPLES * sizeof(int16_t))

#define AUDIO_TASK_STACK_SIZE      4096U
#define AUDIO_TASK_PRIORITY        5U


static const char *TAG = "AUDIO_OUTPUT";

static i2s_chan_handle_t i2s_tx_channel = NULL;

static StreamBufferHandle_t audio_stream_buffer = NULL;

static TaskHandle_t audio_task_handle = NULL;


/*
 * Task riêng dành cho audio output.
 *
 * play_avi() chỉ copy PCM vào Stream Buffer.
 *
 * Task này:
 *
 * Stream Buffer
 *      ↓
 * mono16 -> stereo16
 *      ↓
 * i2s_channel_write()
 *      ↓
 * MAX98357A
 *      ↓
 * loa
 *
 * i2s_channel_write() vẫn có thể block, nhưng chỉ block
 * audio task chứ không block trực tiếp task đang decode video.
 */
static void audio_output_task(void *argument)
{
    (void)argument;

    uint8_t mono_buffer[AUDIO_MONO_BLOCK_BYTES];

    /*
     * 256 mono sample
     *      ↓
     * 256 Left + 256 Right
     */
    int16_t stereo_buffer[AUDIO_MONO_BLOCK_SAMPLES * 2U];


    while (1) {

        /*
         * Chờ đến khi có PCM trong Stream Buffer.
         */
        size_t bytes_received =
            xStreamBufferReceive(
                audio_stream_buffer,
                mono_buffer,
                sizeof(mono_buffer),
                portMAX_DELAY
            );


        if (bytes_received == 0U) {
            continue;
        }


        /*
         * PCM mono16:
         * mỗi sample phải có đúng 2 byte.
         */
        if ((bytes_received % sizeof(int16_t)) != 0U) {

            ESP_LOGE(
                TAG,
                "Odd PCM byte count: %u",
                (unsigned int)bytes_received
            );

            continue;
        }


        size_t mono_samples =
            bytes_received / sizeof(int16_t);


        /*
         * Chuyển:
         *
         * mono:
         *
         * S0 S1 S2 ...
         *
         * thành stereo:
         *
         * L0 R0 L1 R1 L2 R2 ...
         */
        for (
            size_t i = 0U;
            i < mono_samples;
            i++
        ) {

            size_t byte_index =
                i * sizeof(int16_t);


            /*
             * PCM trong AVI là little-endian:
             *
             * byte thấp trước,
             * byte cao sau.
             */
            uint16_t sample_bits =
                (uint16_t)mono_buffer[byte_index] |
                (
                    (uint16_t)
                    mono_buffer[byte_index + 1U]
                    << 8
                );


            int16_t sample =
                (int16_t)sample_bits;


            /*
             * Duplicate mono sang cả Left và Right.
             */
            stereo_buffer[i * 2U] =
                sample;

            stereo_buffer[
                i * 2U + 1U
            ] =
                sample;
        }


        /*
         * Stereo 16-bit:
         *
         * mỗi thời điểm sample có:
         *
         * Left  = 2 byte
         * Right = 2 byte
         *
         * tổng = 4 byte.
         */
        size_t bytes_to_write =
            mono_samples *
            2U *
            sizeof(int16_t);


        size_t bytes_written = 0U;


        esp_err_t error =
            i2s_channel_write(
                i2s_tx_channel,
                stereo_buffer,
                bytes_to_write,
                &bytes_written,
                portMAX_DELAY
            );


        if (error != ESP_OK) {

            ESP_LOGE(
                TAG,
                "PCM I2S task write failed: %s",
                esp_err_to_name(error)
            );

            continue;
        }


        if (bytes_written != bytes_to_write) {

            ESP_LOGW(
                TAG,
                "Partial I2S write: %u/%u bytes",
                (unsigned int)bytes_written,
                (unsigned int)bytes_to_write
            );
        }
    }
}


/*
 * Khởi tạo I2S để ESP32-S3 gửi PCM
 * sang MAX98357A.
 *
 * Đồng thời tạo:
 *
 * - Stream Buffer cho PCM
 * - FreeRTOS audio output task
 */
esp_err_t audio_output_init(void)
{
    /*
     * Không cho init hai lần.
     */
    if (
        i2s_tx_channel != NULL ||
        audio_stream_buffer != NULL ||
        audio_task_handle != NULL
    ) {

        return ESP_ERR_INVALID_STATE;
    }


    /*
     * Tạo một I2S TX channel.
     *
     * TX vì ESP32 chỉ gửi audio ra ngoài.
     */
    i2s_chan_config_t channel_config =
        I2S_CHANNEL_DEFAULT_CONFIG(
            I2S_NUM_AUTO,
            I2S_ROLE_MASTER
        );


    esp_err_t error =
        i2s_new_channel(
            &channel_config,
            &i2s_tx_channel,
            NULL
        );


    if (error != ESP_OK) {

        ESP_LOGE(
            TAG,
            "Cannot create I2S channel: %s",
            esp_err_to_name(error)
        );

        i2s_tx_channel = NULL;

        return error;
    }


    /*
     * Cấu hình I2S:
     *
     * sample rate = 16000 Hz
     * sample      = 16 bit
     * stereo
     */
    i2s_std_config_t i2s_config = {

        .clk_cfg =
            I2S_STD_CLK_DEFAULT_CONFIG(
                AUDIO_SAMPLE_RATE
            ),

        .slot_cfg =
            I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
                I2S_DATA_BIT_WIDTH_16BIT,
                I2S_SLOT_MODE_STEREO
            ),

        .gpio_cfg = {

            .mclk = I2S_GPIO_UNUSED,

            .bclk = AUDIO_PIN_BCLK,

            .ws = AUDIO_PIN_LRC,

            .dout = AUDIO_PIN_DOUT,

            .din = I2S_GPIO_UNUSED,

            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };


    error =
        i2s_channel_init_std_mode(
            i2s_tx_channel,
            &i2s_config
        );


    if (error != ESP_OK) {

        ESP_LOGE(
            TAG,
            "Cannot initialize I2S: %s",
            esp_err_to_name(error)
        );

        i2s_del_channel(
            i2s_tx_channel
        );

        i2s_tx_channel = NULL;

        return error;
    }


    error =
        i2s_channel_enable(
            i2s_tx_channel
        );


    if (error != ESP_OK) {

        ESP_LOGE(
            TAG,
            "Cannot enable I2S: %s",
            esp_err_to_name(error)
        );

        i2s_del_channel(
            i2s_tx_channel
        );

        i2s_tx_channel = NULL;

        return error;
    }


    /*
     * Tạo Stream Buffer chứa PCM mono16.
     *
     * Trigger level = sizeof(int16_t) = 2 byte,
     * vì một PCM sample phải có đủ 2 byte.
     */
    audio_stream_buffer =
        xStreamBufferCreate(
            AUDIO_STREAM_BUFFER_SIZE,
            sizeof(int16_t)
        );


    if (audio_stream_buffer == NULL) {

        ESP_LOGE(
            TAG,
            "Cannot create audio stream buffer"
        );

        i2s_channel_disable(
            i2s_tx_channel
        );

        i2s_del_channel(
            i2s_tx_channel
        );

        i2s_tx_channel = NULL;

        return ESP_ERR_NO_MEM;
    }


    /*
     * Tạo task riêng để lấy PCM từ Stream Buffer
     * và feed sang I2S.
     */
    BaseType_t task_result =
        xTaskCreate(
            audio_output_task,
            "audio_output",
            AUDIO_TASK_STACK_SIZE,
            NULL,
            AUDIO_TASK_PRIORITY,
            &audio_task_handle
        );


    if (task_result != pdPASS) {

        ESP_LOGE(
            TAG,
            "Cannot create audio task"
        );

        vStreamBufferDelete(
            audio_stream_buffer
        );

        audio_stream_buffer = NULL;


        i2s_channel_disable(
            i2s_tx_channel
        );

        i2s_del_channel(
            i2s_tx_channel
        );

        i2s_tx_channel = NULL;

        audio_task_handle = NULL;

        return ESP_ERR_NO_MEM;
    }


    ESP_LOGI(
        TAG,
        "I2S initialized"
    );


    ESP_LOGI(
        TAG,
        "Audio task + %u byte stream buffer ready",
        (unsigned int)AUDIO_STREAM_BUFFER_SIZE
    );


    return ESP_OK;
}


/*
 * Phát square wave để kiểm tra:
 *
 * ESP32-S3
 *   ↓
 * I2S
 *   ↓
 * MAX98357A
 *   ↓
 * loa
 *
 * Hàm test này vẫn ghi trực tiếp xuống I2S.
 * Chỉ nên dùng khi chưa enqueue audio playback.
 */
esp_err_t audio_output_test_tone(void)
{
    if (i2s_tx_channel == NULL) {
        return ESP_ERR_INVALID_STATE;
    }


    /*
     * Nếu Stream Buffer đang có PCM playback,
     * không chen test tone vào giữa.
     */
    if (
        audio_stream_buffer != NULL &&
        xStreamBufferBytesAvailable(
            audio_stream_buffer
        ) > 0U
    ) {

        ESP_LOGW(
            TAG,
            "Cannot play test tone while audio buffer is not empty"
        );

        return ESP_ERR_INVALID_STATE;
    }


    /*
     * 256 sample.
     *
     * Vì đang cấu hình stereo:
     *
     * L0 R0 L1 R1 L2 R2 ...
     *
     * nên cần 256 * 2 giá trị int16_t.
     */
    int16_t audio_buffer[
        AUDIO_MONO_BLOCK_SAMPLES * 2U
    ];


    uint32_t sample_counter = 0U;


    /*
     * Một chu kỳ square wave gồm:
     *
     * HIGH + LOW
     */
    const uint32_t half_period =
        AUDIO_SAMPLE_RATE /
        (TEST_TONE_FREQUENCY * 2U);


    /*
     * Tổng số sample cần phát trong 1 giây.
     */
    const uint32_t total_samples =
        (
            AUDIO_SAMPLE_RATE *
            TEST_TONE_DURATION_MS
        ) /
        1000U;


    uint32_t generated_samples = 0U;


    ESP_LOGI(
        TAG,
        "Playing test tone"
    );


    while (
        generated_samples <
        total_samples
    ) {

        size_t samples_this_block =
            AUDIO_MONO_BLOCK_SAMPLES;


        if (
            generated_samples +
            samples_this_block >
            total_samples
        ) {

            samples_this_block =
                total_samples -
                generated_samples;
        }


        for (
            size_t i = 0U;
            i < samples_this_block;
            i++
        ) {

            /*
             * Biên độ nhỏ để test trước.
             */
            int16_t sample;


            if (
                (
                    sample_counter /
                    half_period
                ) %
                2U ==
                0U
            ) {

                sample = 1500;

            } else {

                sample = -1500;
            }


            sample_counter++;


            /*
             * Gửi cùng sample cho cả
             * Left và Right.
             */
            audio_buffer[i * 2U] =
                sample;

            audio_buffer[
                i * 2U + 1U
            ] =
                sample;
        }


        const size_t bytes_to_write =
            samples_this_block *
            2U *
            sizeof(int16_t);


        size_t bytes_written = 0U;


        esp_err_t error =
            i2s_channel_write(
                i2s_tx_channel,
                audio_buffer,
                bytes_to_write,
                &bytes_written,
                portMAX_DELAY
            );


        if (error != ESP_OK) {

            ESP_LOGE(
                TAG,
                "I2S write failed: %s",
                esp_err_to_name(error)
            );

            return error;
        }


        generated_samples +=
            samples_this_block;
    }


    ESP_LOGI(
        TAG,
        "Test tone completed"
    );


    return ESP_OK;
}


/*
 * Đưa PCM mono16 vào Stream Buffer.
 *
 * Hàm này KHÔNG tự phát audio trực tiếp.
 *
 * play_avi()
 *     ↓
 * audio_output_enqueue_pcm_mono16()
 *     ↓
 * Stream Buffer
 *     ↓
 * audio_output_task()
 *     ↓
 * I2S
 *
 * Nếu buffer còn chỗ, hàm chỉ copy PCM rồi trở về nhanh.
 *
 * Nếu buffer đầy, hàm sẽ chờ đến khi audio task
 * tiêu thụ bớt dữ liệu. Đây là back-pressure cần thiết
 * để tránh ghi đè hoặc làm mất audio.
 */
esp_err_t audio_output_enqueue_pcm_mono16(
    const uint8_t *pcm_data,
    size_t pcm_size
)
{
    if (
        i2s_tx_channel == NULL ||
        audio_stream_buffer == NULL
    ) {

        return ESP_ERR_INVALID_STATE;
    }


    if (
        pcm_data == NULL ||
        pcm_size == 0U
    ) {

        return ESP_ERR_INVALID_ARG;
    }


    /*
     * PCM của AVI hiện tại:
     *
     * - 16-bit
     * - mono
     * - little-endian
     *
     * Một sample phải có đúng 2 byte.
     */
    if (
        (pcm_size % sizeof(int16_t)) != 0U
    ) {

        ESP_LOGE(
            TAG,
            "Invalid PCM size: %u",
            (unsigned int)pcm_size
        );

        return ESP_ERR_INVALID_SIZE;
    }


    size_t offset = 0U;


    while (offset < pcm_size) {

        size_t bytes_remaining =
            pcm_size - offset;


        /*
         * Mỗi lần enqueue tối đa:
         *
         * 256 mono sample
         * = 512 byte.
         */
        size_t bytes_this_block =
            bytes_remaining;


        if (
            bytes_this_block >
            AUDIO_MONO_BLOCK_BYTES
        ) {

            bytes_this_block =
                AUDIO_MONO_BLOCK_BYTES;
        }


        size_t bytes_sent =
            xStreamBufferSend(
                audio_stream_buffer,
                pcm_data + offset,
                bytes_this_block,
                portMAX_DELAY
            );


        if (bytes_sent == 0U) {

            ESP_LOGE(
                TAG,
                "Audio stream send returned 0"
            );

            return ESP_FAIL;
        }


        /*
         * xStreamBufferSend() có thể gửi ít hơn yêu cầu,
         * nên luôn tăng offset đúng số byte thực tế đã copy.
         *
         * Vì dữ liệu PCM phải giữ alignment 16-bit,
         * số byte gửi phải luôn là số chẵn.
         */
        if (
            (bytes_sent % sizeof(int16_t)) != 0U
        ) {

            ESP_LOGE(
                TAG,
                "Audio stream lost 16-bit alignment: %u bytes",
                (unsigned int)bytes_sent
            );

            return ESP_FAIL;
        }


        offset += bytes_sent;
    }


    return ESP_OK;
}
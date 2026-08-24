#include "audio_output.h"

#include <stddef.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"

#include "driver/gpio.h"
#include "driver/i2s_std.h"

#include "esp_log.h"


#define AUDIO_PIN_LRC   GPIO_NUM_4
#define AUDIO_PIN_BCLK  GPIO_NUM_5
#define AUDIO_PIN_DOUT  GPIO_NUM_6

#define AUDIO_SAMPLE_RATE  16000U

#define TEST_TONE_FREQUENCY  440U
#define TEST_TONE_DURATION_MS 1000U


static const char *TAG = "AUDIO_OUTPUT";

static i2s_chan_handle_t i2s_tx_channel = NULL;


/*
 * Khởi tạo I2S để ESP32 gửi PCM
 * sang MAX98357A.
 */
esp_err_t audio_output_init(void)
{
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


    esp_err_t error = i2s_new_channel(
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


    error = i2s_channel_init_std_mode(
        i2s_tx_channel,
        &i2s_config
    );


    if (error != ESP_OK) {

        ESP_LOGE(
            TAG,
            "Cannot initialize I2S: %s",
            esp_err_to_name(error)
        );

        return error;
    }


    error = i2s_channel_enable(
        i2s_tx_channel
    );


    if (error != ESP_OK) {

        ESP_LOGE(
            TAG,
            "Cannot enable I2S: %s",
            esp_err_to_name(error)
        );

        return error;
    }


    ESP_LOGI(
        TAG,
        "I2S initialized"
    );


    return ESP_OK;
}


/*
 * Phát square wave để kiểm tra:
 *
 * ESP32
 *   ↓
 * I2S
 *   ↓
 * MAX98357A
 *   ↓
 * loa
 */
esp_err_t audio_output_test_tone(void)
{
    if (i2s_tx_channel == NULL) {
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
    int16_t audio_buffer[256U * 2U];


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
        (AUDIO_SAMPLE_RATE *
         TEST_TONE_DURATION_MS) /
        1000U;


    uint32_t generated_samples = 0U;


    ESP_LOGI(
        TAG,
        "Playing test tone"
    );


    while (generated_samples < total_samples) {

        size_t samples_this_block = 256U;


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

esp_err_t audio_output_write_pcm_mono16(
    const uint8_t *pcm_data,
    size_t pcm_size
)
{
    if (
        i2s_tx_channel == NULL ||
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
    if (pcm_size % 2U != 0U) {

        ESP_LOGE(
            TAG,
            "Invalid PCM size: %u",
            (unsigned int)pcm_size
        );

        return ESP_ERR_INVALID_SIZE;
    }


    /*
     * Buffer I2S stereo tạm thời.
     *
     * 256 sample mono
     *      ↓
     * 256 Left + 256 Right
     */
    int16_t stereo_buffer[256U * 2U];


    size_t input_offset = 0U;


    while (input_offset < pcm_size) {

        /*
         * Số byte PCM còn lại.
         */
        size_t remaining_bytes =
            pcm_size - input_offset;


        /*
         * Mỗi block xử lý tối đa
         * 256 sample mono.
         *
         * 256 sample × 2 byte = 512 byte.
         */
        size_t mono_samples =
            remaining_bytes / 2U;


        if (mono_samples > 256U) {
            mono_samples = 256U;
        }


        /*
         * Chuyển:
         *
         * mono:
         * S0 S1 S2 ...
         *
         * thành stereo:
         * L0 R0 L1 R1 L2 R2 ...
         */
        for (
            size_t i = 0U;
            i < mono_samples;
            i++
        ) {

            size_t byte_index =
                input_offset +
                i * 2U;


            /*
             * PCM trong AVI là little-endian.
             *
             * byte thấp trước,
             * byte cao sau.
             */
            uint16_t sample_bits =
                (uint16_t)pcm_data[byte_index] |
                (
                    (uint16_t)
                    pcm_data[byte_index + 1U]
                    << 8
                );


            int16_t sample =
                (int16_t)sample_bits;


            /*
             * Duplicate mono sang
             * cả Left và Right.
             */
            stereo_buffer[i * 2U] =
                sample;

            stereo_buffer[
                i * 2U + 1U
            ] =
                sample;
        }


        /*
         * Stereo:
         *
         * mỗi sample thời gian có:
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
                "PCM I2S write failed: %s",
                esp_err_to_name(error)
            );

            return error;
        }


        /*
         * Mỗi mono sample đã tiêu thụ
         * 2 byte input.
         */
        input_offset +=
            mono_samples * 2U;
    }


    return ESP_OK;
}
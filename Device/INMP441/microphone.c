#include "microphone.h"

#include "freertos/FreeRTOS.h"
#include <stddef.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "driver/i2s_std.h"

#include "esp_log.h"

#define MICROPHONE_BCLK_GPIO   GPIO_NUM_15
#define MICROPHONE_WS_GPIO     GPIO_NUM_16
#define MICROPHONE_DATA_GPIO   GPIO_NUM_17

#define MICROPHONE_RAW_BUFFER_SAMPLES 256U

static const char *TAG = "MICROPHONE";

static i2s_chan_handle_t microphone_rx_channel = NULL;

/*
 * Buffer nhận raw data từ I2S.
 *
 * INMP441 gửi sample có độ rộng lớn hơn 16-bit,
 * nên ban đầu ta nhận dưới dạng int32_t rồi
 * chuyển về PCM int16_t.
 */
static int32_t raw_buffer[MICROPHONE_RAW_BUFFER_SAMPLES];

esp_err_t microphone_init(void)
{
    if (microphone_rx_channel != NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    /*
     * Tạo I2S RX channel.
     *
     * ESP32-S3 đóng vai trò master:
     * - tạo BCLK
     * - tạo WS
     *
     * INMP441 gửi data về ESP32.
     */
    i2s_chan_config_t channel_config =
        I2S_CHANNEL_DEFAULT_CONFIG(
            I2S_NUM_AUTO,
            I2S_ROLE_MASTER
        );


    esp_err_t error =
        i2s_new_channel(
            &channel_config,
            NULL,
            &microphone_rx_channel
        );


    if (error != ESP_OK) {

        ESP_LOGE(
            TAG,
            "Cannot create I2S RX channel: %s",
            esp_err_to_name(error)
        );

        microphone_rx_channel = NULL;

        return error;
    }


    /*
     * INMP441 dùng chuẩn I2S Philips.
     *
     * Ta nhận slot 32-bit.
     *
     * L/R của INMP441 nối GND,
     * vì vậy microphone nằm ở LEFT channel.
     */
    i2s_std_config_t i2s_config = {

        .clk_cfg =
            I2S_STD_CLK_DEFAULT_CONFIG(
                MICROPHONE_SAMPLE_RATE
            ),

        .slot_cfg =
            I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
                I2S_DATA_BIT_WIDTH_32BIT,
                I2S_SLOT_MODE_MONO
            ),

        .gpio_cfg = {

            .mclk = I2S_GPIO_UNUSED,

            .bclk = MICROPHONE_BCLK_GPIO,

            .ws = MICROPHONE_WS_GPIO,

            .dout = I2S_GPIO_UNUSED,

            .din = MICROPHONE_DATA_GPIO,

            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };


    /*
     * Vì L/R nối GND nên chọn LEFT slot.
     */
    i2s_config.slot_cfg.slot_mask =
        I2S_STD_SLOT_LEFT;


    error =
        i2s_channel_init_std_mode(
            microphone_rx_channel,
            &i2s_config
        );


    if (error != ESP_OK) {

        ESP_LOGE(
            TAG,
            "Cannot initialize microphone I2S: %s",
            esp_err_to_name(error)
        );

        i2s_del_channel(
            microphone_rx_channel
        );

        microphone_rx_channel = NULL;

        return error;
    }


    error =
        i2s_channel_enable(
            microphone_rx_channel
        );


    if (error != ESP_OK) {

        ESP_LOGE(
            TAG,
            "Cannot enable microphone I2S: %s",
            esp_err_to_name(error)
        );

        i2s_del_channel(
            microphone_rx_channel
        );

        microphone_rx_channel = NULL;

        return error;
    }


    ESP_LOGI(
        TAG,
        "INMP441 initialized"
    );

    ESP_LOGI(
        TAG,
        "Sample rate: %u Hz",
        MICROPHONE_SAMPLE_RATE
    );

    ESP_LOGI(
        TAG,
        "BCLK=%d WS=%d DATA=%d",
        MICROPHONE_BCLK_GPIO,
        MICROPHONE_WS_GPIO,
        MICROPHONE_DATA_GPIO
    );


    return ESP_OK;
}


esp_err_t microphone_read(
    int16_t *samples,
    size_t sample_count,
    size_t *samples_read
)
{
    if (microphone_rx_channel == NULL) {
        return ESP_ERR_INVALID_STATE;
    }


    if (
        samples == NULL ||
        samples_read == NULL ||
        sample_count == 0U
    ) {
        return ESP_ERR_INVALID_ARG;
    }


    *samples_read = 0U;


    size_t output_offset = 0U;


    while (output_offset < sample_count) {

        size_t samples_remaining =
            sample_count - output_offset;


        size_t samples_this_read =
            samples_remaining;


        if (
            samples_this_read >
            MICROPHONE_RAW_BUFFER_SAMPLES
        ) {

            samples_this_read =
                MICROPHONE_RAW_BUFFER_SAMPLES;
        }


        const size_t bytes_requested =
            samples_this_read *
            sizeof(int32_t);


        size_t bytes_read = 0U;


        esp_err_t error =
            i2s_channel_read(
                microphone_rx_channel,
                raw_buffer,
                bytes_requested,
                &bytes_read,
                portMAX_DELAY
            );


        if (error != ESP_OK) {

            ESP_LOGE(
                TAG,
                "I2S microphone read failed: %s",
                esp_err_to_name(error)
            );

            return error;
        }


        size_t raw_samples =
            bytes_read / sizeof(int32_t);


        /*
         * Chuyển raw 32-bit về PCM 16-bit.
         *
         * Đây là scale ban đầu.
         *
         * Nếu sau khi test biên độ quá nhỏ hoặc clipping,
         * ta sẽ điều chỉnh shift dựa trên dữ liệu thật.
         */
        for (
            size_t i = 0U;
            i < raw_samples;
            i++
        ) {

            int32_t raw_sample =
                raw_buffer[i];


            int32_t pcm_sample =
                raw_sample >> 16;


            if (pcm_sample > INT16_MAX) {
                pcm_sample = INT16_MAX;
            }

            if (pcm_sample < INT16_MIN) {
                pcm_sample = INT16_MIN;
            }


            samples[output_offset + i] =
                (int16_t)pcm_sample;
        }


        output_offset +=
            raw_samples;


        if (raw_samples == 0U) {
            break;
        }
    }


    *samples_read =
        output_offset;


    return ESP_OK;
}


esp_err_t microphone_deinit(void)
{
    if (microphone_rx_channel == NULL) {
        return ESP_OK;
    }


    esp_err_t error =
        i2s_channel_disable(
            microphone_rx_channel
        );


    if (error != ESP_OK) {

        ESP_LOGE(
            TAG,
            "Cannot disable microphone: %s",
            esp_err_to_name(error)
        );

        return error;
    }


    error =
        i2s_del_channel(
            microphone_rx_channel
        );


    if (error != ESP_OK) {

        ESP_LOGE(
            TAG,
            "Cannot delete microphone channel: %s",
            esp_err_to_name(error)
        );

        return error;
    }


    microphone_rx_channel = NULL;


    ESP_LOGI(
        TAG,
        "Microphone deinitialized"
    );


    return ESP_OK;
}
#include "image_decoder.h"

/*
 * Header của component espressif/esp_jpeg
 */
#include "jpeg_decoder.h"

#include "esp_log.h"

static const char *TAG = "IMAGE_DECODER";

esp_err_t jpeg_decoder_decode_rgb565(
    const uint8_t *jpeg_data,
    size_t jpeg_size,
    uint8_t *rgb565_buffer,
    size_t rgb565_buffer_size,
    uint16_t *output_width,
    uint16_t *output_height
)
{
    if (
        jpeg_data == NULL ||
        jpeg_size == 0 ||
        rgb565_buffer == NULL ||
        rgb565_buffer_size == 0
    ) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_jpeg_image_cfg_t jpeg_config = {
        /*
         * Dữ liệu JPEG nén đọc từ AVI / SD card
         */
        .indata = (uint8_t *)jpeg_data,
        .indata_size = jpeg_size,

        /*
         * Buffer RGB565 đầu ra
         */
        .outbuf = rgb565_buffer,
        .outbuf_size = rgb565_buffer_size,

        /*
         * Xuất ra định dạng RGB565
         */
        .out_format = JPEG_IMAGE_FORMAT_RGB565,

        /*
         * Không scale khi decode
         */
        .out_scale = JPEG_IMAGE_SCALE_0,

        .flags = {
            /*
             * SSD1351 thường cần byte cao trước
             */
            .swap_color_bytes = 1,
        },
    };

    esp_jpeg_image_output_t output_info = {0};

    esp_err_t error = esp_jpeg_decode(
        &jpeg_config,
        &output_info
    );

    if (error != ESP_OK) {
        ESP_LOGE(
            TAG,
            "JPEG decode failed: %s",
            esp_err_to_name(error)
        );
        return error;
    }

    ESP_LOGD(
        TAG,
        "Decoded JPEG: %u x %u",
        (unsigned int)output_info.width,
        (unsigned int)output_info.height
    );

    if (
        output_info.width != JPEG_OUTPUT_WIDTH ||
        output_info.height != JPEG_OUTPUT_HEIGHT
    ) {
        ESP_LOGE(
            TAG,
            "Unexpected JPEG size: %u x %u, expected %u x %u",
            (unsigned int)output_info.width,
            (unsigned int)output_info.height,
            (unsigned int)JPEG_OUTPUT_WIDTH,
            (unsigned int)JPEG_OUTPUT_HEIGHT
        );

        return ESP_ERR_INVALID_SIZE;
    }

    if (output_width != NULL) {
        *output_width = (uint16_t)output_info.width;
    }

    if (output_height != NULL) {
        *output_height = (uint16_t)output_info.height;
    }

    return ESP_OK;
}
#ifndef IMAGE_DECODER_H
#define IMAGE_DECODER_H

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define JPEG_OUTPUT_WIDTH   128
#define JPEG_OUTPUT_HEIGHT  128

#define JPEG_RGB565_BUFFER_SIZE \
    (JPEG_OUTPUT_WIDTH * JPEG_OUTPUT_HEIGHT * 2U)

/*
 * Giải mã JPEG thành RGB565.
 *
 * jpeg_data:
 *     Buffer chứa dữ liệu JPEG nén.
 *
 * jpeg_size:
 *     Số byte JPEG.
 *
 * rgb565_buffer:
 *     Buffer đầu ra RGB565.
 *
 * rgb565_buffer_size:
 *     Kích thước buffer đầu ra.
 *
 * output_width, output_height:
 *     Kích thước ảnh thực tế sau giải mã.
 */
esp_err_t jpeg_decoder_decode_rgb565(
    const uint8_t *jpeg_data,
    size_t jpeg_size,
    uint8_t *rgb565_buffer,
    size_t rgb565_buffer_size,
    uint16_t *output_width,
    uint16_t *output_height
);

#endif
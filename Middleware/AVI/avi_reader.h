#ifndef AVI_READER_H
#define AVI_READER_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "esp_err.h"


typedef struct {
    FILE *file;

    long movi_start;
    long movi_end;

    uint32_t frame_index;
} avi_stream_t;

typedef struct
{
    uint32_t stream_index;

    uint16_t format_tag;
    uint16_t channels;

    uint32_t sample_rate;
    uint32_t avg_bytes_per_sec;

    uint16_t block_align;
    uint16_t bits_per_sample;

} avi_audio_info_t;

typedef enum
{
    AVI_CHUNK_VIDEO = 0,
    AVI_CHUNK_AUDIO

} avi_chunk_type_t;

esp_err_t avi_stream_read_next_chunk(
    avi_stream_t *stream,
    uint8_t *buffer,
    size_t buffer_capacity,
    avi_chunk_type_t *chunk_type,
    size_t *chunk_size
);

esp_err_t avi_find_movi(
    const char *file_path,
    long *movi_offset,
    uint32_t *movi_size
);


esp_err_t avi_inspect_movi_start(
    const char *file_path,
    long movi_offset
);


esp_err_t avi_read_first_jpeg(
    const char *file_path,
    long movi_offset,
    uint8_t *jpeg_buffer,
    size_t buffer_capacity,
    size_t *jpeg_size
);


/*
 * Mở file và giữ file mở để đọc tuần tự.
 */
esp_err_t avi_stream_open(
    avi_stream_t *stream,
    const char *file_path,
    long movi_offset,
    uint32_t movi_size
);


/*
 * Tìm và đọc JPEG frame tiếp theo.
 *
 * Trả về:
 * ESP_OK            : đọc được một frame
 * ESP_ERR_NOT_FOUND : đã hết vùng movi
 */
esp_err_t avi_stream_read_next_jpeg(
    avi_stream_t *stream,
    uint8_t *jpeg_buffer,
    size_t buffer_capacity,
    size_t *jpeg_size
);


esp_err_t avi_stream_rewind(
    avi_stream_t *stream
);


void avi_stream_close(
    avi_stream_t *stream
);

esp_err_t avi_get_audio_info(
    const char *file_path,
    avi_audio_info_t *audio_info
);

esp_err_t avi_get_frame_interval(
    const char *file_path,
    uint32_t *microseconds_per_frame
);
#endif
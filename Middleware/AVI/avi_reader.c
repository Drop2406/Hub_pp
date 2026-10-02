#include "avi_reader.h"

#include <stdbool.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"


#define AVI_FILE_BUFFER_SIZE (32U * 1024U)


static const char *TAG = "AVI_READER";

/*
 * Chỉ dùng một AVI stream tại một thời điểm.
 * Buffer này không nằm trên stack.
 */
static uint8_t avi_file_buffer[AVI_FILE_BUFFER_SIZE];


static uint32_t read_u32_le(const uint8_t *data)
{
    return ((uint32_t)data[0])
         | ((uint32_t)data[1] << 8)
         | ((uint32_t)data[2] << 16)
         | ((uint32_t)data[3] << 24);
}

static uint16_t read_u16_le(const uint8_t *data)
{
    return (uint16_t)(
        ((uint16_t)data[0]) |
        ((uint16_t)data[1] << 8)
    );
}

/*
 * read_exact() phải được khai báo trước discard_exact(),
 * vì discard_exact() gọi hàm này.
 */
static esp_err_t read_exact(
    FILE *file,
    void *buffer,
    size_t size
)
{
    if (file == NULL || buffer == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t bytes_read = fread(
        buffer,
        1,
        size,
        file
    );

    if (bytes_read != size) {
        return ESP_ERR_INVALID_SIZE;
    }

    return ESP_OK;
}


/*
 * Đọc bỏ dữ liệu thay vì dùng fseek().
 *
 * Cách này giữ luồng fread() tuần tự và tận dụng được
 * buffer 32 KB đã cấu hình bằng setvbuf().
 */
static esp_err_t discard_exact(
    FILE *file,
    size_t size
)
{
    if (file == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t discard_buffer[128];

    while (size > 0U) {
        size_t block_size =
            size < sizeof(discard_buffer)
            ? size
            : sizeof(discard_buffer);

        esp_err_t error = read_exact(
            file,
            discard_buffer,
            block_size
        );

        if (error != ESP_OK) {
            return error;
        }

        size -= block_size;
    }

    return ESP_OK;
}


/*
 * Chỉ dùng trong giai đoạn tìm LIST movi.
 * Sau khi stream đã mở, hàm đọc frame không dùng fseek()
 * để bỏ qua payload nữa.
 */
static esp_err_t seek_forward_aligned(
    FILE *file,
    uint32_t payload_size
)
{
    long skip_size = (long)payload_size;

    if ((payload_size & 1U) != 0U) {
        skip_size++;
    }

    if (fseek(file, skip_size, SEEK_CUR) != 0) {
        return ESP_FAIL;
    }

    return ESP_OK;
}


esp_err_t avi_find_movi(
    const char *file_path,
    long *movi_offset,
    uint32_t *movi_size
)
{
    if (
        file_path == NULL ||
        movi_offset == NULL ||
        movi_size == NULL
    ) {
        return ESP_ERR_INVALID_ARG;
    }

    FILE *file = fopen(file_path, "rb");

    if (file == NULL) {
        ESP_LOGE(
            TAG,
            "Cannot open file: %s",
            file_path
        );

        return ESP_FAIL;
    }

    uint8_t riff_header[12];

    esp_err_t error = read_exact(
        file,
        riff_header,
        sizeof(riff_header)
    );

    if (error != ESP_OK) {
        ESP_LOGE(TAG, "Cannot read RIFF header");

        fclose(file);
        return error;
    }

    if (
        memcmp(&riff_header[0], "RIFF", 4) != 0 ||
        memcmp(&riff_header[8], "AVI ", 4) != 0
    ) {
        ESP_LOGE(TAG, "Invalid AVI file");

        fclose(file);
        return ESP_ERR_INVALID_RESPONSE;
    }

    while (1) {
        uint8_t chunk_header[8];

        error = read_exact(
            file,
            chunk_header,
            sizeof(chunk_header)
        );

        if (error != ESP_OK) {
            ESP_LOGE(TAG, "LIST movi not found");

            fclose(file);
            return ESP_ERR_NOT_FOUND;
        }

        uint32_t chunk_size =
            read_u32_le(&chunk_header[4]);

        if (memcmp(chunk_header, "LIST", 4) == 0) {
            if (chunk_size < 4U) {
                fclose(file);
                return ESP_ERR_INVALID_SIZE;
            }

            uint8_t list_type[4];

            error = read_exact(
                file,
                list_type,
                sizeof(list_type)
            );

            if (error != ESP_OK) {
                fclose(file);
                return error;
            }

            ESP_LOGI(
                TAG,
                "Found LIST type: %c%c%c%c, size: %" PRIu32,
                list_type[0],
                list_type[1],
                list_type[2],
                list_type[3],
                chunk_size
            );

            if (memcmp(list_type, "movi", 4) == 0) {
                *movi_offset = ftell(file);
                *movi_size = chunk_size - 4U;

                ESP_LOGI(TAG, "LIST movi found");
                ESP_LOGI(
                    TAG,
                    "movi offset: %ld",
                    *movi_offset
                );
                ESP_LOGI(
                    TAG,
                    "movi data size: %" PRIu32 " bytes",
                    *movi_size
                );

                fclose(file);
                return ESP_OK;
            }

            /*
             * Đã đọc 4 byte list_type.
             */
            long bytes_to_skip =
                (long)(chunk_size - 4U);

            if ((chunk_size & 1U) != 0U) {
                bytes_to_skip++;
            }

            if (fseek(
                    file,
                    bytes_to_skip,
                    SEEK_CUR
                ) != 0) {
                ESP_LOGE(TAG, "Cannot skip LIST chunk");

                fclose(file);
                return ESP_FAIL;
            }

            continue;
        }

        error = seek_forward_aligned(
            file,
            chunk_size
        );

        if (error != ESP_OK) {
            ESP_LOGE(TAG, "Cannot skip AVI chunk");

            fclose(file);
            return error;
        }
    }
}


esp_err_t avi_inspect_movi_start(
    const char *file_path,
    long movi_offset
)
{
    if (file_path == NULL || movi_offset < 0) {
        return ESP_ERR_INVALID_ARG;
    }

    FILE *file = fopen(file_path, "rb");

    if (file == NULL) {
        ESP_LOGE(
            TAG,
            "Cannot open AVI file: %s",
            file_path
        );

        return ESP_FAIL;
    }

    if (fseek(file, movi_offset, SEEK_SET) != 0) {
        fclose(file);
        return ESP_FAIL;
    }

    uint8_t chunk_header[8];

    esp_err_t error = read_exact(
        file,
        chunk_header,
        sizeof(chunk_header)
    );

    if (error != ESP_OK) {
        fclose(file);
        return error;
    }

    uint32_t chunk_size =
        read_u32_le(&chunk_header[4]);

    ESP_LOGI(
        TAG,
        "First movi item ID: %.4s",
        (char *)chunk_header
    );

    ESP_LOGI(
        TAG,
        "First movi item size: %" PRIu32 " bytes",
        chunk_size
    );

    if (memcmp(chunk_header, "LIST", 4) == 0) {
        uint8_t list_type[4];

        error = read_exact(
            file,
            list_type,
            sizeof(list_type)
        );

        if (error == ESP_OK) {
            ESP_LOGI(
                TAG,
                "Nested LIST type: %c%c%c%c",
                list_type[0],
                list_type[1],
                list_type[2],
                list_type[3]
            );
        }

        fclose(file);
        return error;
    }

    bool is_video_chunk =
        chunk_header[2] == 'd' &&
        (
            chunk_header[3] == 'c' ||
            chunk_header[3] == 'b'
        );

    if (!is_video_chunk) {
        ESP_LOGI(
            TAG,
            "First movi item is not a video chunk"
        );

        fclose(file);
        return ESP_OK;
    }

    uint8_t first_data_bytes[2];

    error = read_exact(
        file,
        first_data_bytes,
        sizeof(first_data_bytes)
    );

    if (error != ESP_OK) {
        fclose(file);
        return error;
    }

    ESP_LOGI(
        TAG,
        "First video data bytes: %02X %02X",
        first_data_bytes[0],
        first_data_bytes[1]
    );

    if (
        first_data_bytes[0] == 0xFF &&
        first_data_bytes[1] == 0xD8
    ) {
        ESP_LOGI(
            TAG,
            "First video chunk contains a JPEG frame"
        );
    } else {
        ESP_LOGW(
            TAG,
            "Video chunk does not begin with JPEG marker FF D8"
        );
    }

    fclose(file);
    return ESP_OK;
}


esp_err_t avi_read_first_jpeg(
    const char *file_path,
    long movi_offset,
    uint8_t *jpeg_buffer,
    size_t buffer_capacity,
    size_t *jpeg_size
)
{
    if (
        file_path == NULL ||
        jpeg_buffer == NULL ||
        jpeg_size == NULL ||
        movi_offset < 0
    ) {
        return ESP_ERR_INVALID_ARG;
    }

    FILE *file = fopen(file_path, "rb");

    if (file == NULL) {
        return ESP_FAIL;
    }

    if (fseek(file, movi_offset, SEEK_SET) != 0) {
        fclose(file);
        return ESP_FAIL;
    }

    uint8_t chunk_header[8];

    esp_err_t error = read_exact(
        file,
        chunk_header,
        sizeof(chunk_header)
    );

    if (error != ESP_OK) {
        fclose(file);
        return error;
    }

    bool is_video_chunk =
        chunk_header[2] == 'd' &&
        (
            chunk_header[3] == 'c' ||
            chunk_header[3] == 'b'
        );

    if (!is_video_chunk) {
        fclose(file);
        return ESP_ERR_NOT_SUPPORTED;
    }

    uint32_t chunk_size =
        read_u32_le(&chunk_header[4]);

    if (chunk_size > buffer_capacity) {
        fclose(file);
        return ESP_ERR_NO_MEM;
    }

    if (chunk_size < 4U) {
        fclose(file);
        return ESP_ERR_INVALID_SIZE;
    }

    error = read_exact(
        file,
        jpeg_buffer,
        chunk_size
    );

    fclose(file);

    if (error != ESP_OK) {
        return error;
    }

    if (
        jpeg_buffer[0] != 0xFF ||
        jpeg_buffer[1] != 0xD8
    ) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    *jpeg_size = chunk_size;

    return ESP_OK;
}


esp_err_t avi_stream_open(
    avi_stream_t *stream,
    const char *file_path,
    long movi_offset,
    uint32_t movi_size
)
{
    if (
        stream == NULL ||
        file_path == NULL ||
        movi_offset < 0 ||
        movi_size == 0U
    ) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(stream, 0, sizeof(*stream));

    stream->file = fopen(file_path, "rb");

    if (stream->file == NULL) {
        ESP_LOGE(
            TAG,
            "Cannot open AVI stream: %s",
            file_path
        );

        return ESP_FAIL;
    }

    /*
     * setvbuf() phải được gọi trước thao tác đọc/seek đầu tiên
     * trên stream vừa mở.
     */
    int buffer_result = setvbuf(
        stream->file,
        (char *)avi_file_buffer,
        _IOFBF,
        sizeof(avi_file_buffer)
    );

    if (buffer_result != 0) {
        ESP_LOGW(
            TAG,
            "Cannot configure 32 KB AVI file buffer"
        );
    } else {
        ESP_LOGI(
            TAG,
            "AVI file buffer: %u bytes",
            (unsigned int)sizeof(avi_file_buffer)
        );
    }

    if (fseek(
            stream->file,
            movi_offset,
            SEEK_SET
        ) != 0) {
        fclose(stream->file);
        stream->file = NULL;

        return ESP_FAIL;
    }

    stream->movi_start = movi_offset;
    stream->movi_end =
        movi_offset + (long)movi_size;

    stream->frame_index = 0U;

    ESP_LOGI(
        TAG,
        "AVI stream opened, movi range: %ld to %ld",
        stream->movi_start,
        stream->movi_end
    );

    return ESP_OK;
}

esp_err_t avi_stream_read_next_chunk(
    avi_stream_t *stream,
    uint8_t *buffer,
    size_t buffer_capacity,
    avi_chunk_type_t *chunk_type,
    size_t *chunk_size
)
{
    if (
        stream == NULL ||
        stream->file == NULL ||
        buffer == NULL ||
        buffer_capacity == 0U ||
        chunk_type == NULL ||
        chunk_size == NULL
    ) {
        return ESP_ERR_INVALID_ARG;
    }


    *chunk_size = 0U;


    while (1) {

        long current_position =
            ftell(stream->file);


        /*
         * Không còn đủ 8 byte để đọc
         * một chunk header mới.
         */
        if (
            current_position < 0 ||
            current_position + 8L >
                stream->movi_end
        ) {
            return ESP_ERR_NOT_FOUND;
        }


        /*
         * Chunk header:
         *
         * 4 byte ID
         * 4 byte size
         */
        uint8_t chunk_header[8];


        esp_err_t error = read_exact(
            stream->file,
            chunk_header,
            sizeof(chunk_header)
        );


        if (error != ESP_OK) {
            return error;
        }


        uint32_t data_size =
            read_u32_le(
                &chunk_header[4]
            );


        /*
         * Có thể gặp LIST rec
         * nằm bên trong movi.
         */
        if (
            memcmp(
                chunk_header,
                "LIST",
                4
            ) == 0
        ) {

            if (data_size < 4U) {
                return ESP_ERR_INVALID_SIZE;
            }


            uint8_t list_type[4];


            error = read_exact(
                stream->file,
                list_type,
                sizeof(list_type)
            );


            if (error != ESP_OK) {
                return error;
            }


            /*
             * LIST rec chứa các chunk media
             * ở bên trong.
             *
             * Không bỏ cả LIST.
             * Tiếp tục đọc chunk con.
             */
            if (
                memcmp(
                    list_type,
                    "rec ",
                    4
                ) == 0
            ) {
                continue;
            }


            /*
             * LIST khác thì bỏ qua phần còn lại.
             *
             * data_size đã bao gồm 4 byte list_type.
             */
            error = discard_exact(
                stream->file,
                data_size - 4U
            );


            if (error != ESP_OK) {
                return error;
            }


            /*
             * RIFF padding.
             */
            if (data_size & 1U) {

                error = discard_exact(
                    stream->file,
                    1U
                );


                if (error != ESP_OK) {
                    return error;
                }
            }


            continue;
        }


        /*
         * Ví dụ:
         *
         * 00dc → video compressed
         * 00db → video
         */
        bool is_video_chunk =
            chunk_header[2] == 'd' &&
            (
                chunk_header[3] == 'c' ||
                chunk_header[3] == 'b'
            );


        /*
         * Ví dụ file hiện tại:
         *
         * 01wb → audio PCM
         */
        bool is_audio_chunk =
            chunk_header[2] == 'w' &&
            chunk_header[3] == 'b';


        /*
         * Không phải video cũng không phải audio
         * → bỏ qua.
         */
        if (
            !is_video_chunk &&
            !is_audio_chunk
        ) {

            error = discard_exact(
                stream->file,
                data_size
            );


            if (error != ESP_OK) {
                return error;
            }


            if (data_size & 1U) {

                error = discard_exact(
                    stream->file,
                    1U
                );


                if (error != ESP_OK) {
                    return error;
                }
            }


            continue;
        }


        /*
         * Kiểm tra buffer có đủ chứa chunk không.
         */
        if (data_size > buffer_capacity) {

            ESP_LOGE(
                TAG,
                "Chunk too large: %u > %u",
                (unsigned int)data_size,
                (unsigned int)buffer_capacity
            );


            return ESP_ERR_NO_MEM;
        }


        /*
         * Đọc payload của video/audio vào RAM.
         */
        error = read_exact(
            stream->file,
            buffer,
            data_size
        );


        if (error != ESP_OK) {
            return error;
        }


        /*
         * RIFF padding:
         *
         * payload lẻ byte thì sau nó có
         * thêm 1 padding byte.
         */
        if (data_size & 1U) {

            error = discard_exact(
                stream->file,
                1U
            );


            if (error != ESP_OK) {
                return error;
            }
        }


        /*
         * VIDEO
         */
        if (is_video_chunk) {

            /*
             * JPEG phải bắt đầu bằng FF D8.
             */
            if (
                data_size < 2U ||
                buffer[0] != 0xFFU ||
                buffer[1] != 0xD8U
            ) {

                ESP_LOGE(
                    TAG,
                    "Invalid JPEG start marker"
                );


                return ESP_ERR_INVALID_RESPONSE;
            }


            /*
             * Kiểm tra marker kết thúc JPEG.
             */
            if (
                data_size >= 2U &&
                (
                    buffer[data_size - 2U] != 0xFFU ||
                    buffer[data_size - 1U] != 0xD9U
                )
            ) {

                ESP_LOGW(
                    TAG,
                    "JPEG does not end with FF D9"
                );
            }


            *chunk_type =
                AVI_CHUNK_VIDEO;


            stream->frame_index++;
        }


        /*
         * AUDIO
         */
        else {

            *chunk_type =
                AVI_CHUNK_AUDIO;
        }


        *chunk_size =
            (size_t)data_size;


        return ESP_OK;
    }
}

esp_err_t avi_stream_read_next_jpeg(
    avi_stream_t *stream,
    uint8_t *jpeg_buffer,
    size_t buffer_capacity,
    size_t *jpeg_size
)
{
    if (
        stream == NULL ||
        stream->file == NULL ||
        jpeg_buffer == NULL ||
        jpeg_size == NULL
    ) {
        return ESP_ERR_INVALID_ARG;
    }

    *jpeg_size = 0U;

    while (1) {
        long current_position =
            ftell(stream->file);

        if (
            current_position < 0 ||
            current_position + 8L > stream->movi_end
        ) {
            return ESP_ERR_NOT_FOUND;
        }

        uint8_t chunk_header[8];

        esp_err_t error = read_exact(
            stream->file,
            chunk_header,
            sizeof(chunk_header)
        );

        if (error != ESP_OK) {
            return ESP_ERR_NOT_FOUND;
        }

        uint32_t chunk_size =
            read_u32_le(&chunk_header[4]);

        long payload_start =
            ftell(stream->file);

        long payload_end =
            payload_start +
            (long)chunk_size +
            (long)(chunk_size & 1U);

        if (
            payload_start < 0 ||
            payload_end > stream->movi_end
        ) {
            ESP_LOGE(
                TAG,
                "Chunk %.4s exceeds LIST movi boundary",
                (char *)chunk_header
            );

            return ESP_ERR_INVALID_SIZE;
        }

        /*
         * Một số AVI bọc frame trong LIST rec .
         *
         * Khi gặp LIST rec, đã đọc 4 byte list_type rồi
         * tiếp tục đọc các chunk con theo thứ tự tuần tự.
         */
        if (memcmp(chunk_header, "LIST", 4) == 0) {
            if (chunk_size < 4U) {
                return ESP_ERR_INVALID_SIZE;
            }

            uint8_t list_type[4];

            error = read_exact(
                stream->file,
                list_type,
                sizeof(list_type)
            );

            if (error != ESP_OK) {
                return error;
            }

            if (memcmp(list_type, "rec ", 4) == 0) {
                continue;
            }

            error = discard_exact(
                stream->file,
                chunk_size - 4U
            );

            if (error != ESP_OK) {
                return error;
            }

            if ((chunk_size & 1U) != 0U) {
                error = discard_exact(
                    stream->file,
                    1U
                );

                if (error != ESP_OK) {
                    return error;
                }
            }

            continue;
        }

        bool is_video_chunk =
            chunk_header[2] == 'd' &&
            (
                chunk_header[3] == 'c' ||
                chunk_header[3] == 'b'
            );

        bool is_audio_chunk =
            chunk_header[2] == 'w' &&
            chunk_header[3] == 'b';
        
        // if (is_audio_chunk) {
        //     ESP_LOGI(
        //         TAG,
        //         "Audio chunk: %c%c%c%c, size: %u bytes",
        //         chunk_header[0],
        //         chunk_header[1],
        //         chunk_header[2],
        //         chunk_header[3],
        //         (unsigned int)chunk_size
        //     );
        // }

        if (!is_video_chunk) {
            error = discard_exact(
                stream->file,
                chunk_size
            );

            if (error != ESP_OK) {
                return error;
            }

            if ((chunk_size & 1U) != 0U) {
                error = discard_exact(
                    stream->file,
                    1U
                );

                if (error != ESP_OK) {
                    return error;
                }
            }

            continue;
        }

        if (chunk_size > buffer_capacity) {
            ESP_LOGE(
                TAG,
                "Frame %" PRIu32
                " too large: %" PRIu32
                " bytes, capacity: %u",
                stream->frame_index + 1U,
                chunk_size,
                (unsigned int)buffer_capacity
            );

            return ESP_ERR_NO_MEM;
        }

        if (chunk_size < 4U) {
            return ESP_ERR_INVALID_SIZE;
        }

        error = read_exact(
            stream->file,
            jpeg_buffer,
            chunk_size
        );

        if (error != ESP_OK) {
            return error;
        }

        /*
         * Đọc bỏ padding byte, không dùng fseek().
         */
        if ((chunk_size & 1U) != 0U) {
            error = discard_exact(
                stream->file,
                1U
            );

            if (error != ESP_OK) {
                return error;
            }
        }

        if (
            jpeg_buffer[0] != 0xFF ||
            jpeg_buffer[1] != 0xD8
        ) {
            ESP_LOGW(
                TAG,
                "Chunk %.4s is not a JPEG frame",
                (char *)chunk_header
            );

            continue;
        }

        if (
            jpeg_buffer[chunk_size - 2U] != 0xFF ||
            jpeg_buffer[chunk_size - 1U] != 0xD9
        ) {
            ESP_LOGW(
                TAG,
                "Frame %" PRIu32
                " does not end with JPEG marker FF D9",
                stream->frame_index + 1U
            );
        }

        *jpeg_size = chunk_size;
        stream->frame_index++;

        return ESP_OK;
    }
}


esp_err_t avi_stream_rewind(
    avi_stream_t *stream
)
{
    if (
        stream == NULL ||
        stream->file == NULL
    ) {
        return ESP_ERR_INVALID_ARG;
    }

    /*
     * Rewind chỉ diễn ra khi phát lại từ đầu,
     * không nằm trong vòng đọc từng frame.
     */
    if (fseek(
            stream->file,
            stream->movi_start,
            SEEK_SET
        ) != 0) {
        return ESP_FAIL;
    }

    stream->frame_index = 0U;

    return ESP_OK;
}


void avi_stream_close(
    avi_stream_t *stream
)
{
    if (stream == NULL) {
        return;
    }

    if (stream->file != NULL) {
        fclose(stream->file);
        stream->file = NULL;
    }

    stream->movi_start = 0;
    stream->movi_end = 0;
    stream->frame_index = 0U;
}

static esp_err_t parse_audio_stream_list(
    FILE *file,
    long list_end,
    uint32_t stream_index,
    avi_audio_info_t *audio_info,
    bool *audio_found
)
{
    bool is_audio_stream = false;

    while (1) {

        long current_position =
            ftell(file);

        if (
            current_position < 0 ||
            current_position + 8L > list_end
        ) {
            break;
        }


        uint8_t chunk_header[8];

        esp_err_t error = read_exact(
            file,
            chunk_header,
            sizeof(chunk_header)
        );

        if (error != ESP_OK) {
            return error;
        }


        uint32_t chunk_size =
            read_u32_le(
                &chunk_header[4]
            );


        /*
         * strh = stream header.
         *
         * 4 byte đầu của payload cho biết
         * loại stream:
         *
         * vids = video
         * auds = audio
         */
        if (
            memcmp(
                chunk_header,
                "strh",
                4
            ) == 0
        ) {

            if (chunk_size < 4U) {
                return ESP_ERR_INVALID_SIZE;
            }


            uint8_t stream_type[4];

            error = read_exact(
                file,
                stream_type,
                sizeof(stream_type)
            );

            if (error != ESP_OK) {
                return error;
            }


            is_audio_stream =
                memcmp(
                    stream_type,
                    "auds",
                    4
                ) == 0;


            /*
             * Bỏ phần còn lại của strh.
             */
            error = discard_exact(
                file,
                chunk_size - 4U
            );

            if (error != ESP_OK) {
                return error;
            }
        }


        /*
         * strf của audio chứa WAVEFORMAT.
         */
        else if (
            memcmp(
                chunk_header,
                "strf",
                4
            ) == 0 &&
            is_audio_stream
        ) {

            if (chunk_size < 16U) {
                return ESP_ERR_INVALID_SIZE;
            }


            uint8_t format_data[16];

            error = read_exact(
                file,
                format_data,
                sizeof(format_data)
            );

            if (error != ESP_OK) {
                return error;
            }


            audio_info->stream_index =
                stream_index;

            audio_info->format_tag =
                read_u16_le(
                    &format_data[0]
                );

            audio_info->channels =
                read_u16_le(
                    &format_data[2]
                );

            audio_info->sample_rate =
                read_u32_le(
                    &format_data[4]
                );

            audio_info->avg_bytes_per_sec =
                read_u32_le(
                    &format_data[8]
                );

            audio_info->block_align =
                read_u16_le(
                    &format_data[12]
                );

            audio_info->bits_per_sample =
                read_u16_le(
                    &format_data[14]
                );


            /*
             * Nếu strf dài hơn 16 byte,
             * bỏ phần mở rộng.
             */
            if (chunk_size > 16U) {

                error = discard_exact(
                    file,
                    chunk_size - 16U
                );

                if (error != ESP_OK) {
                    return error;
                }
            }


            /*
             * RIFF padding.
             */
            if (chunk_size & 1U) {

                error = discard_exact(
                    file,
                    1U
                );

                if (error != ESP_OK) {
                    return error;
                }
            }


            *audio_found = true;

            return ESP_OK;
        }


        /*
         * Chunk khác → bỏ.
         */
        else {

            error = discard_exact(
                file,
                chunk_size
            );

            if (error != ESP_OK) {
                return error;
            }
        }


        /*
         * Padding cho chunk vừa đọc.
         */
        if (chunk_size & 1U) {

            error = discard_exact(
                file,
                1U
            );

            if (error != ESP_OK) {
                return error;
            }
        }
    }


    return ESP_OK;
}

esp_err_t avi_get_audio_info(
    const char *file_path,
    avi_audio_info_t *audio_info
)
{
    if (
        file_path == NULL ||
        audio_info == NULL
    ) {
        return ESP_ERR_INVALID_ARG;
    }


    memset(
        audio_info,
        0,
        sizeof(*audio_info)
    );


    FILE *file =
        fopen(file_path, "rb");

    if (file == NULL) {
        return ESP_FAIL;
    }


    uint8_t riff_header[12];

    esp_err_t error = read_exact(
        file,
        riff_header,
        sizeof(riff_header)
    );


    if (error != ESP_OK) {
        fclose(file);
        return error;
    }


    if (
        memcmp(
            &riff_header[0],
            "RIFF",
            4
        ) != 0 ||
        memcmp(
            &riff_header[8],
            "AVI ",
            4
        ) != 0
    ) {

        fclose(file);

        return ESP_ERR_INVALID_RESPONSE;
    }


    uint32_t stream_index = 0U;


    while (1) {

        uint8_t chunk_header[8];


        size_t bytes_read =
            fread(
                chunk_header,
                1,
                sizeof(chunk_header),
                file
            );


        if (bytes_read == 0U) {
            break;
        }


        if (
            bytes_read !=
            sizeof(chunk_header)
        ) {

            fclose(file);

            return ESP_ERR_INVALID_SIZE;
        }


        uint32_t chunk_size =
            read_u32_le(
                &chunk_header[4]
            );


        /*
         * Ta quan tâm các LIST.
         */
        if (
            memcmp(
                chunk_header,
                "LIST",
                4
            ) == 0
        ) {

            if (chunk_size < 4U) {

                fclose(file);

                return ESP_ERR_INVALID_SIZE;
            }


            uint8_t list_type[4];

            error = read_exact(
                file,
                list_type,
                sizeof(list_type)
            );


            if (error != ESP_OK) {

                fclose(file);

                return error;
            }


            /*
             * LIST hdrl chứa các LIST strl.
             *
             * Ta sẽ duyệt phần này riêng ở bước kế.
             */
            if (
                memcmp(
                    list_type,
                    "hdrl",
                    4
                ) == 0
            ) {

                long hdrl_end =
                    ftell(file) +
                    (long)(
                        chunk_size - 4U
                    );


                while (ftell(file) < hdrl_end) {

                    uint8_t hdrl_chunk[8];

                    error = read_exact(
                        file,
                        hdrl_chunk,
                        sizeof(hdrl_chunk)
                    );


                    if (error != ESP_OK) {
                        fclose(file);
                        return error;
                    }


                    uint32_t hdrl_chunk_size =
                        read_u32_le(
                            &hdrl_chunk[4]
                        );


                    if (
                        memcmp(
                            hdrl_chunk,
                            "LIST",
                            4
                        ) == 0
                    ) {

                        if (
                            hdrl_chunk_size <
                            4U
                        ) {
                            fclose(file);
                            return ESP_ERR_INVALID_SIZE;
                        }


                        uint8_t hdrl_list_type[4];

                        error = read_exact(
                            file,
                            hdrl_list_type,
                            4
                        );


                        if (error != ESP_OK) {
                            fclose(file);
                            return error;
                        }


                        if (
                            memcmp(
                                hdrl_list_type,
                                "strl",
                                4
                            ) == 0
                        ) {

                            long strl_end =
                                ftell(file) +
                                (long)(
                                    hdrl_chunk_size -
                                    4U
                                );


                            bool audio_found =
                                false;


                            error =
                                parse_audio_stream_list(
                                    file,
                                    strl_end,
                                    stream_index,
                                    audio_info,
                                    &audio_found
                                );


                            if (error != ESP_OK) {
                                fclose(file);
                                return error;
                            }


                            /*
                             * Đảm bảo đứng cuối LIST strl.
                             */
                            if (
                                ftell(file) <
                                strl_end
                            ) {

                                error =
                                    discard_exact(
                                        file,
                                        (size_t)(
                                            strl_end -
                                            ftell(file)
                                        )
                                    );


                                if (
                                    error !=
                                    ESP_OK
                                ) {
                                    fclose(file);
                                    return error;
                                }
                            }


                            if (
                                hdrl_chunk_size &
                                1U
                            ) {

                                error =
                                    discard_exact(
                                        file,
                                        1U
                                    );

                                if (
                                    error !=
                                    ESP_OK
                                ) {
                                    fclose(file);
                                    return error;
                                }
                            }


                            if (audio_found) {

                                fclose(file);

                                return ESP_OK;
                            }


                            /*
                             * Một LIST strl =
                             * một stream.
                             */
                            stream_index++;
                        }

                        else {

                            error =
                                discard_exact(
                                    file,
                                    hdrl_chunk_size -
                                    4U
                                );

                            if (
                                error !=
                                ESP_OK
                            ) {
                                fclose(file);
                                return error;
                            }


                            if (
                                hdrl_chunk_size &
                                1U
                            ) {
                                error =
                                    discard_exact(
                                        file,
                                        1U
                                    );

                                if (
                                    error !=
                                    ESP_OK
                                ) {
                                    fclose(file);
                                    return error;
                                }
                            }
                        }
                    }

                    else {

                        error = discard_exact(
                            file,
                            hdrl_chunk_size
                        );

                        if (error != ESP_OK) {
                            fclose(file);
                            return error;
                        }


                        if (
                            hdrl_chunk_size &
                            1U
                        ) {

                            error = discard_exact(
                                file,
                                1U
                            );

                            if (
                                error !=
                                ESP_OK
                            ) {
                                fclose(file);
                                return error;
                            }
                        }
                    }
                }
            }

            else {

                error = discard_exact(
                    file,
                    chunk_size - 4U
                );

                if (error != ESP_OK) {
                    fclose(file);
                    return error;
                }


                if (chunk_size & 1U) {

                    error =
                        discard_exact(
                            file,
                            1U
                        );

                    if (
                        error !=
                        ESP_OK
                    ) {
                        fclose(file);
                        return error;
                    }
                }
            }
        }

        else {

            error = discard_exact(
                file,
                chunk_size
            );

            if (error != ESP_OK) {
                fclose(file);
                return error;
            }


            if (chunk_size & 1U) {

                error =
                    discard_exact(
                        file,
                        1U
                    );

                if (error != ESP_OK) {
                    fclose(file);
                    return error;
                }
            }
        }
    }


    fclose(file);

    return ESP_ERR_NOT_FOUND;
}

esp_err_t avi_get_frame_interval(
    const char *file_path,
    uint32_t *microseconds_per_frame
)
{
    if (
        file_path == NULL ||
        microseconds_per_frame == NULL
    ) {
        return ESP_ERR_INVALID_ARG;
    }


    FILE *file = fopen(
        file_path,
        "rb"
    );


    if (file == NULL) {

        ESP_LOGE(
            TAG,
            "Cannot open AVI: %s",
            file_path
        );

        return ESP_FAIL;
    }


    /*
     * RIFF header:
     *
     * "RIFF"
     * size
     * "AVI "
     */
    uint8_t riff_header[12];


    esp_err_t error = read_exact(
        file,
        riff_header,
        sizeof(riff_header)
    );


    if (error != ESP_OK) {

        fclose(file);
        return error;
    }


    if (
        memcmp(riff_header, "RIFF", 4) != 0 ||
        memcmp(&riff_header[8], "AVI ", 4) != 0
    ) {

        fclose(file);
        return ESP_ERR_INVALID_RESPONSE;
    }


    /*
     * Tìm LIST hdrl.
     */
    while (1) {

        uint8_t chunk_header[8];


        error = read_exact(
            file,
            chunk_header,
            sizeof(chunk_header)
        );


        if (error != ESP_OK) {

            fclose(file);
            return ESP_ERR_NOT_FOUND;
        }


        uint32_t chunk_size =
            read_u32_le(
                &chunk_header[4]
            );


        if (
            memcmp(
                chunk_header,
                "LIST",
                4
            ) == 0
        ) {

            if (chunk_size < 4U) {

                fclose(file);
                return ESP_ERR_INVALID_SIZE;
            }


            uint8_t list_type[4];


            error = read_exact(
                file,
                list_type,
                sizeof(list_type)
            );


            if (error != ESP_OK) {

                fclose(file);
                return error;
            }


            /*
             * Tìm đúng LIST hdrl.
             */
            if (
                memcmp(
                    list_type,
                    "hdrl",
                    4
                ) == 0
            ) {

                long hdrl_end =
                    ftell(file) +
                    (long)(chunk_size - 4U);


                /*
                 * Tìm chunk avih bên trong hdrl.
                 */
                while (
                    ftell(file) + 8L <= hdrl_end
                ) {

                    uint8_t hdrl_chunk[8];


                    error = read_exact(
                        file,
                        hdrl_chunk,
                        sizeof(hdrl_chunk)
                    );


                    if (error != ESP_OK) {

                        fclose(file);
                        return error;
                    }


                    uint32_t hdrl_chunk_size =
                        read_u32_le(
                            &hdrl_chunk[4]
                        );


                    /*
                     * avih = Main AVI Header.
                     *
                     * 4 byte đầu payload chính là
                     * dwMicroSecPerFrame.
                     */
                    if (
                        memcmp(
                            hdrl_chunk,
                            "avih",
                            4
                        ) == 0
                    ) {

                        if (hdrl_chunk_size < 4U) {

                            fclose(file);
                            return ESP_ERR_INVALID_SIZE;
                        }


                        uint8_t value[4];


                        error = read_exact(
                            file,
                            value,
                            sizeof(value)
                        );


                        if (error != ESP_OK) {

                            fclose(file);
                            return error;
                        }


                        *microseconds_per_frame =
                            read_u32_le(value);


                        fclose(file);

                        return ESP_OK;
                    }


                    /*
                     * Không phải avih → bỏ payload.
                     */
                    error = discard_exact(
                        file,
                        hdrl_chunk_size
                    );


                    if (error != ESP_OK) {

                        fclose(file);
                        return error;
                    }


                    /*
                     * RIFF padding.
                     */
                    if (hdrl_chunk_size & 1U) {

                        error = discard_exact(
                            file,
                            1U
                        );


                        if (error != ESP_OK) {

                            fclose(file);
                            return error;
                        }
                    }
                }


                fclose(file);

                return ESP_ERR_NOT_FOUND;
            }


            /*
             * LIST khác → bỏ phần còn lại.
             *
             * 4 byte list_type đã đọc rồi.
             */
            error = discard_exact(
                file,
                chunk_size - 4U
            );


            if (error != ESP_OK) {

                fclose(file);
                return error;
            }
        }

        else {

            /*
             * Chunk bình thường → bỏ payload.
             */
            error = discard_exact(
                file,
                chunk_size
            );


            if (error != ESP_OK) {

                fclose(file);
                return error;
            }
        }


        /*
         * RIFF chunk được căn theo 2 byte.
         */
        if (chunk_size & 1U) {

            error = discard_exact(
                file,
                1U
            );


            if (error != ESP_OK) {

                fclose(file);
                return error;
            }
        }
    }
}
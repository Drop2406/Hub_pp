#ifndef SSD1351_H
#define SSD1351_H

#include <stdint.h>
#include "esp_err.h"

#define SSD1351_WIDTH   128
#define SSD1351_HEIGHT  128

#define SSD1351_FRAME_BYTES \
    (SSD1351_WIDTH * SSD1351_HEIGHT * 2U)

/*
 * Chuyển màu RGB888 sang RGB565.
 *
 * Ví dụ:
 * SSD1351_RGB565(255, 0, 0) -> màu đỏ
 */
#define SSD1351_RGB565(r, g, b)                     \
    ((uint16_t)((((uint16_t)(r) & 0xF8U) << 8) |   \
                (((uint16_t)(g) & 0xFCU) << 3) |   \
                (((uint16_t)(b)) >> 3)))

/*
 * Khởi tạo GPIO, SPI và SSD1351.
 */
esp_err_t ssd1351_init(void);

/*
 * Tô một hình chữ nhật.
 *
 * x0, y0: góc trên bên trái
 * x1, y1: góc dưới bên phải
 */
esp_err_t ssd1351_fill_rect(
    uint8_t x0,
    uint8_t y0,
    uint8_t x1,
    uint8_t y1,
    uint16_t color
);

/*
 * Tô toàn bộ màn hình bằng một màu.
 */
esp_err_t ssd1351_fill_screen(uint16_t color);

/*
 * Hiển thị một frame RGB565 kích thước 128x128.
 *
 * Mỗi pixel gồm hai byte:
 * byte cao trước, byte thấp sau.
 *
 * Buffer phải có đúng SSD1351_FRAME_BYTES byte.
 */
esp_err_t ssd1351_draw_rgb565_frame(
    const uint8_t *frame
);

esp_err_t ssd1351_fill_screen_slow_test(
    uint16_t color
);

/*
 * Bắt đầu truyền một framebuffer bằng SPI DMA
 * rồi trả về ngay, không chờ truyền xong.
 *
 * Frame phải nằm trong DMA-capable RAM.
 */
esp_err_t ssd1351_queue_rgb565_frame(
    const uint8_t *frame
);

/*
 * Chờ framebuffer đã queue truyền xong.
 *
 * Nếu hiện không có frame đang truyền,
 * hàm trả về ESP_OK.
 */
esp_err_t ssd1351_wait_frame_done(void);
#endif
#include "ssd1351.h"

#include <stdlib.h>
#include <stddef.h>
#include <string.h>
#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "driver/spi_master.h"

#include "esp_log.h"


/* ==================== Pin configuration ==================== */

#define PIN_SCL    12
#define PIN_SDA    11
#define PIN_RES    10
#define PIN_DC      9
#define PIN_CS      8


/* ==================== SSD1351 commands ==================== */

#define CMD_SETCOLUMN       0x15
#define CMD_SETROW          0x75
#define CMD_WRITERAM        0x5C
#define CMD_SETREMAP        0xA0
#define CMD_STARTLINE       0xA1
#define CMD_DISPLAYOFFSET   0xA2
#define CMD_NORMALDISPLAY   0xA6
#define CMD_SETVSL          0xB4
#define CMD_SETGPIO         0xB5
#define CMD_SETPRECHARGE    0xB1
#define CMD_VCOMH           0xBE
#define CMD_CONTRASTABC     0xC1
#define CMD_CONTRASTMASTER  0xC7
#define CMD_MUXRATIO        0xCA
#define CMD_COMMANDLOCK     0xFD
#define CMD_DISPLAYON       0xAF
#define CMD_DISPLAYOFF      0xAE
#define CMD_PRECHARGE2      0xB6
#define CMD_CLOCKDIV        0xB3


/* ==================== Internal variables ==================== */

static const char *TAG = "SSD1351";

static spi_device_handle_t spi;
static uint8_t *dma_line_buffer;
static spi_transaction_t frame_transaction;
static bool frame_in_flight = false;

/* ==================== Internal functions ==================== */

static esp_err_t ssd1351_send_command(uint8_t command)
{
    gpio_set_level(PIN_DC, 0);

    spi_transaction_t transaction = {
        .length = 8,
        .tx_buffer = &command,
    };

    return spi_device_polling_transmit(
        spi,
        &transaction
    );
}


static esp_err_t ssd1351_send_data_byte(uint8_t data)
{
    gpio_set_level(PIN_DC, 1);

    spi_transaction_t transaction = {
        .length = 8,
        .tx_buffer = &data,
    };

    return spi_device_polling_transmit(
        spi,
        &transaction
    );
}


static void ssd1351_reset(void)
{
    gpio_set_level(PIN_RES, 1);
    vTaskDelay(pdMS_TO_TICKS(50));

    gpio_set_level(PIN_RES, 0);
    vTaskDelay(pdMS_TO_TICKS(50));

    gpio_set_level(PIN_RES, 1);
    vTaskDelay(pdMS_TO_TICKS(50));
}


static esp_err_t ssd1351_set_window(
    uint8_t x0,
    uint8_t y0,
    uint8_t x1,
    uint8_t y1
)
{
    esp_err_t error;

    error = ssd1351_send_command(CMD_SETCOLUMN);
    if (error != ESP_OK) {
        return error;
    }

    error = ssd1351_send_data_byte(x0);
    if (error != ESP_OK) {
        return error;
    }

    error = ssd1351_send_data_byte(x1);
    if (error != ESP_OK) {
        return error;
    }

    error = ssd1351_send_command(CMD_SETROW);
    if (error != ESP_OK) {
        return error;
    }

    error = ssd1351_send_data_byte(y0);
    if (error != ESP_OK) {
        return error;
    }

    error = ssd1351_send_data_byte(y1);
    if (error != ESP_OK) {
        return error;
    }

    return ssd1351_send_command(CMD_WRITERAM);
}


static esp_err_t ssd1351_controller_init(void)
{
    esp_err_t error;

    ssd1351_reset();

    error = ssd1351_send_command(CMD_COMMANDLOCK);
    if (error != ESP_OK) {
        return error;
    }

    error = ssd1351_send_data_byte(0x12);
    if (error != ESP_OK) {
        return error;
    }

    error = ssd1351_send_command(CMD_COMMANDLOCK);
    if (error != ESP_OK) {
        return error;
    }

    error = ssd1351_send_data_byte(0xB1);
    if (error != ESP_OK) {
        return error;
    }

    error = ssd1351_send_command(CMD_DISPLAYOFF);
    if (error != ESP_OK) {
        return error;
    }

    error = ssd1351_send_command(CMD_CLOCKDIV);
    if (error != ESP_OK) {
        return error;
    }

    error = ssd1351_send_data_byte(0xF1);
    if (error != ESP_OK) {
        return error;
    }

    error = ssd1351_send_command(CMD_MUXRATIO);
    if (error != ESP_OK) {
        return error;
    }

    error = ssd1351_send_data_byte(127);
    if (error != ESP_OK) {
        return error;
    }

    error = ssd1351_send_command(CMD_DISPLAYOFFSET);
    if (error != ESP_OK) {
        return error;
    }

    error = ssd1351_send_data_byte(0x00);
    if (error != ESP_OK) {
        return error;
    }

    error = ssd1351_send_command(CMD_STARTLINE);
    if (error != ESP_OK) {
        return error;
    }

    error = ssd1351_send_data_byte(0x00);
    if (error != ESP_OK) {
        return error;
    }

    /*
     * 0x74:
     * - RGB color
     * - 16-bit color depth
     * - Increment horizontal address
     */
    error = ssd1351_send_command(CMD_SETREMAP);
    if (error != ESP_OK) {
        return error;
    }

    error = ssd1351_send_data_byte(0x74);
    if (error != ESP_OK) {
        return error;
    }

    error = ssd1351_send_command(CMD_SETGPIO);
    if (error != ESP_OK) {
        return error;
    }

    error = ssd1351_send_data_byte(0x00);
    if (error != ESP_OK) {
        return error;
    }

    error = ssd1351_send_command(CMD_SETVSL);
    if (error != ESP_OK) {
        return error;
    }

    error = ssd1351_send_data_byte(0xA0);
    if (error != ESP_OK) {
        return error;
    }

    error = ssd1351_send_data_byte(0xB5);
    if (error != ESP_OK) {
        return error;
    }

    error = ssd1351_send_data_byte(0x55);
    if (error != ESP_OK) {
        return error;
    }

    error = ssd1351_send_command(CMD_CONTRASTABC);
    if (error != ESP_OK) {
        return error;
    }

    error = ssd1351_send_data_byte(0xC8);
    if (error != ESP_OK) {
        return error;
    }

    error = ssd1351_send_data_byte(0x80);
    if (error != ESP_OK) {
        return error;
    }

    error = ssd1351_send_data_byte(0xC8);
    if (error != ESP_OK) {
        return error;
    }

    error = ssd1351_send_command(CMD_CONTRASTMASTER);
    if (error != ESP_OK) {
        return error;
    }

    error = ssd1351_send_data_byte(0x0F);
    if (error != ESP_OK) {
        return error;
    }

    error = ssd1351_send_command(CMD_SETPRECHARGE);
    if (error != ESP_OK) {
        return error;
    }

    error = ssd1351_send_data_byte(0x32);
    if (error != ESP_OK) {
        return error;
    }

    error = ssd1351_send_command(CMD_PRECHARGE2);
    if (error != ESP_OK) {
        return error;
    }

    error = ssd1351_send_data_byte(0x01);
    if (error != ESP_OK) {
        return error;
    }

    error = ssd1351_send_command(CMD_VCOMH);
    if (error != ESP_OK) {
        return error;
    }

    error = ssd1351_send_data_byte(0x05);
    if (error != ESP_OK) {
        return error;
    }

    error = ssd1351_send_command(CMD_NORMALDISPLAY);
    if (error != ESP_OK) {
        return error;
    }

    error = ssd1351_send_command(CMD_DISPLAYON);
    if (error != ESP_OK) {
        return error;
    }

    ESP_LOGI(TAG, "OLED controller initialized");

    return ESP_OK;
}


/* ==================== Public functions ==================== */

esp_err_t ssd1351_init(void)
{
    gpio_config_t io_config = {
        .pin_bit_mask =
            (1ULL << PIN_RES) |
            (1ULL << PIN_DC) |
            (1ULL << PIN_CS),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    esp_err_t error = gpio_config(&io_config);
    if (error != ESP_OK) {
        ESP_LOGE(
            TAG,
            "GPIO configuration failed: %s",
            esp_err_to_name(error)
        );

        return error;
    }

    /*
     * Driver hiện tại không dùng CS tự động.
     * OLED luôn được chọn bằng cách giữ CS ở mức thấp.
     */
    gpio_set_level(PIN_CS, 0);

    spi_bus_config_t bus_config = {
        .mosi_io_num = PIN_SDA,
        .miso_io_num = -1,
        .sclk_io_num = PIN_SCL,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,

        /*
         * Mỗi dòng LCD:
         * 128 pixel x 2 byte = 256 byte.
         */
        .max_transfer_sz = SSD1351_FRAME_BYTES,
    };

    error = spi_bus_initialize(
        SPI2_HOST,
        &bus_config,
        SPI_DMA_CH_AUTO
    );

    if (error != ESP_OK) {
        ESP_LOGE(
            TAG,
            "SPI bus initialization failed: %s",
            esp_err_to_name(error)
        );

        return error;
    }

    dma_line_buffer = spi_bus_dma_memory_alloc(
        SPI2_HOST,
        SSD1351_WIDTH * 2U,
        0
    );

    if (dma_line_buffer == NULL) {
        ESP_LOGE(TAG, "Cannot allocate SPI DMA line buffer");
        return ESP_ERR_NO_MEM;
    }

    spi_device_interface_config_t device_config = {
        .clock_speed_hz = 10 * 1000 * 1000,
        .mode = 0,
        .spics_io_num = -1,
        .queue_size = 2,
    };

    error = spi_bus_add_device(
        SPI2_HOST,
        &device_config,
        &spi
    );

    if (error != ESP_OK) {
        ESP_LOGE(
            TAG,
            "SPI device initialization failed: %s",
            esp_err_to_name(error)
        );

        return error;
    }

    error = ssd1351_controller_init();

    if (error != ESP_OK) {
        ESP_LOGE(
            TAG,
            "SSD1351 controller initialization failed: %s",
            esp_err_to_name(error)
        );

        return error;
    }

    ESP_LOGI(TAG, "SSD1351 initialization complete");

    return ESP_OK;
}


esp_err_t ssd1351_fill_rect(
    uint8_t x0,
    uint8_t y0,
    uint8_t x1,
    uint8_t y1,
    uint16_t color
)
{
    if (x0 > x1 || y0 > y1) {
        return ESP_ERR_INVALID_ARG;
    }

    if (x1 >= SSD1351_WIDTH || y1 >= SSD1351_HEIGHT) {
        return ESP_ERR_INVALID_ARG;
    }

    const size_t width = x1 - x0 + 1U;
    const size_t height = y1 - y0 + 1U;
    const size_t line_bytes = width * 2U;

    /*
     * SSD1351 nhận RGB565 theo byte cao trước.
     */
    for (size_t x = 0; x < width; x++) {
        dma_line_buffer[x * 2U] =
            (uint8_t)(color >> 8);

        dma_line_buffer[x * 2U + 1U] =
            (uint8_t)(color & 0xFFU);
    }

    esp_err_t error = ssd1351_set_window(
        x0,
        y0,
        x1,
        y1
    );

    if (error != ESP_OK) {
        return error;
    }

    gpio_set_level(PIN_DC, 1);

    for (size_t y = 0; y < height; y++) {
        spi_transaction_t transaction = {
            .length = line_bytes * 8U,
            .tx_buffer = dma_line_buffer,
        };

        error = spi_device_polling_transmit(
            spi,
            &transaction
        );

        if (error != ESP_OK) {
            return error;
        }
    }

    return ESP_OK;
}


esp_err_t ssd1351_fill_screen(uint16_t color)
{
    return ssd1351_fill_rect(
        0,
        0,
        SSD1351_WIDTH - 1,
        SSD1351_HEIGHT - 1,
        color
    );
}


esp_err_t ssd1351_draw_rgb565_frame(
    const uint8_t *frame
)
{
    if (frame == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t error = ssd1351_set_window(
        0,
        0,
        SSD1351_WIDTH - 1U,
        SSD1351_HEIGHT - 1U
    );

    if (error != ESP_OK) {
        return error;
    }

    gpio_set_level(PIN_DC, 1);

    /*
     * Gửi toàn bộ frame bằng đúng một SPI DMA transaction.
     *
     * frame bắt buộc phải nằm trong vùng RAM hỗ trợ DMA.
     */
    spi_transaction_t transaction = {
        .length = SSD1351_FRAME_BYTES * 8U,
        .tx_buffer = frame,
    };

    error = spi_device_polling_transmit(
        spi,
        &transaction
    );

    if (error != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Full-frame transfer failed: %s",
            esp_err_to_name(error)
        );

        return error;
    }

    return ESP_OK;
}

esp_err_t ssd1351_fill_screen_slow_test(uint16_t color)
{
    esp_err_t error = ssd1351_set_window(
        0,
        0,
        SSD1351_WIDTH - 1,
        SSD1351_HEIGHT - 1
    );

    if (error != ESP_OK) {
        return error;
    }

    gpio_set_level(PIN_DC, 1);

    /*
     * Gửi hai pixel trong mỗi transaction.
     *
     * Dữ liệu nằm ngay trong spi_transaction_t.tx_data,
     * không dùng line_buffer, framebuffer hoặc malloc().
     */
    spi_transaction_t transaction = {0};

    transaction.flags = SPI_TRANS_USE_TXDATA;
    transaction.length = 32;  // 4 byte = 2 pixel RGB565

    const uint8_t high_byte =
        (uint8_t)(color >> 8);

    const uint8_t low_byte =
        (uint8_t)(color & 0xFFU);

    transaction.tx_data[0] = high_byte;
    transaction.tx_data[1] = low_byte;
    transaction.tx_data[2] = high_byte;
    transaction.tx_data[3] = low_byte;

    const size_t pixel_pairs =
        (SSD1351_WIDTH * SSD1351_HEIGHT) / 2U;

    for (size_t i = 0; i < pixel_pairs; i++) {
        error = spi_device_polling_transmit(
            spi,
            &transaction
        );

        if (error != ESP_OK) {
            ESP_LOGE(
                TAG,
                "Slow test failed at pair %u: %s",
                (unsigned int)i,
                esp_err_to_name(error)
            );

            return error;
        }
    }

    ESP_LOGI(
        TAG,
        "Slow solid-color test completed"
    );

    return ESP_OK;
}

esp_err_t ssd1351_wait_frame_done(void)
{
    if (!frame_in_flight) {
        return ESP_OK;
    }

    spi_transaction_t *completed_transaction = NULL;

    esp_err_t error = spi_device_get_trans_result(
        spi,
        &completed_transaction,
        portMAX_DELAY
    );

    if (error != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Waiting for LCD DMA failed: %s",
            esp_err_to_name(error)
        );

        return error;
    }

    if (completed_transaction != &frame_transaction) {
        ESP_LOGE(
            TAG,
            "Unexpected completed SPI transaction"
        );

        return ESP_ERR_INVALID_RESPONSE;
    }

    frame_in_flight = false;

    return ESP_OK;
}


esp_err_t ssd1351_queue_rgb565_frame(
    const uint8_t *frame
)
{
    if (frame == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /*
     * Không được sửa hoặc sử dụng lại transaction descriptor
     * khi transaction trước vẫn đang chạy.
     */
    if (frame_in_flight) {
        return ESP_ERR_INVALID_STATE;
    }

    /*
     * Không còn transaction nào đang chạy nên có thể dùng
     * các polling transaction để gửi lệnh set window.
     */
    esp_err_t error = ssd1351_set_window(
        0,
        0,
        SSD1351_WIDTH - 1U,
        SSD1351_HEIGHT - 1U
    );

    if (error != ESP_OK) {
        return error;
    }

    gpio_set_level(PIN_DC, 1);

    memset(
        &frame_transaction,
        0,
        sizeof(frame_transaction)
    );

    frame_transaction.length =
        SSD1351_FRAME_BYTES * 8U;

    frame_transaction.tx_buffer =
        frame;

    error = spi_device_queue_trans(
        spi,
        &frame_transaction,
        portMAX_DELAY
    );

    if (error != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Queueing LCD DMA frame failed: %s",
            esp_err_to_name(error)
        );

        return error;
    }

    frame_in_flight = true;

    return ESP_OK;
}
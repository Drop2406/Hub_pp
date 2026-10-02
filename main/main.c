#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#include "esp_err.h"
#include "esp_log.h"
#include "esp_heap_caps.h"

#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_event.h"

#include "ssd1351.h"
#include "audio_output.h"
#include "sd_card.h"
#include "wifi_sta.h"

#include "app_media.h"
#include "app_led.h"
#include "app_voice.h"


static const char *TAG = "MAIN";

static EventGroupHandle_t s_wifi_events = NULL;


static void print_internal_ram(const char *stage)
{
    ESP_LOGI(
        TAG,
        "[RAM] %s: free=%u, largest=%u bytes",
        stage,
        (unsigned)heap_caps_get_free_size(
            MALLOC_CAP_INTERNAL |
            MALLOC_CAP_8BIT
        ),
        (unsigned)heap_caps_get_largest_free_block(
            MALLOC_CAP_INTERNAL |
            MALLOC_CAP_8BIT
        )
    );
}


static void start_wifi(void)
{
    esp_err_t err;

    err = nvs_flash_init();

    if (err != ESP_OK) {
        goto fail;
    }

    err = esp_netif_init();

    if (err != ESP_OK) {
        goto fail;
    }

    err = esp_event_loop_create_default();

    if (
        err != ESP_OK &&
        err != ESP_ERR_INVALID_STATE
    ) {
        goto fail;
    }

    s_wifi_events =
        xEventGroupCreate();

    if (s_wifi_events == NULL) {
        err = ESP_ERR_NO_MEM;
        goto fail;
    }

    err = wifi_sta_init(
        s_wifi_events
    );

    if (err != ESP_OK) {
        goto fail;
    }

    ESP_LOGI(
        TAG,
        "Wi-Fi initialized; waiting for IP"
    );

    return;

fail:

    ESP_LOGE(
        TAG,
        "Wi-Fi startup failed: %s",
        esp_err_to_name(err)
    );
}


void app_main(void)
{
    ESP_ERROR_CHECK(
        ssd1351_init()
    );

    esp_err_t error =
        app_media_init();

    if (error != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Media init failed: %s",
            esp_err_to_name(error)
        );

        return;
    }

    ESP_ERROR_CHECK(
        audio_output_init()
    );

    ESP_ERROR_CHECK(
        ssd1351_fill_screen(
            SSD1351_RGB565(0, 0, 0)
        )
    );

    ESP_ERROR_CHECK(
        sd_card_mount()
    );

    print_internal_ram("BOOT");
    print_internal_ram("BEFORE WIFI");

    /*
     * Wi-Fi phải khởi động trước ESP-SR.
     * Thứ tự này đã giải quyết vấn đề thiếu
     * internal RAM trước đây.
     */
    start_wifi();

    print_internal_ram("AFTER WIFI");

    ESP_ERROR_CHECK(
        app_led_init()
    );

    error = app_voice_init();

    if (error != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Voice init failed: %s",
            esp_err_to_name(error)
        );

        return;
    }

    print_internal_ram("HUB READY");

    while (1) {
        vTaskDelay(
            pdMS_TO_TICKS(1000)
        );
    }
}
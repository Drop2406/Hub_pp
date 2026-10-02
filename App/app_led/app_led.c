#include "app_led.h"

#include "driver/gpio.h"

#define LED_PIN GPIO_NUM_43

esp_err_t app_led_init(void)
{
    esp_err_t error = gpio_reset_pin(LED_PIN);

    if (error != ESP_OK) {
        return error;
    }

    error = gpio_set_level(LED_PIN, 1);

    if (error != ESP_OK) {
        return error;
    }

    error = gpio_set_direction(
        LED_PIN,
        GPIO_MODE_OUTPUT
    );

    if (error != ESP_OK) {
        return error;
    }

    /* Active-low: 1 = OFF. */
    return gpio_set_level(LED_PIN, 1);
}

void app_led_set(bool enabled)
{
    /* Active-low: 0 = ON, 1 = OFF. */
    gpio_set_level(
        LED_PIN,
        enabled ? 0 : 1
    );
}
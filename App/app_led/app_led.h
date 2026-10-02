#pragma once

#include <stdbool.h>
#include "esp_err.h"

esp_err_t app_led_init(void);
void app_led_set(bool enabled);
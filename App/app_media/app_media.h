#pragma once

#include "esp_err.h"

esp_err_t app_media_init(void);

esp_err_t app_media_play(
    const char *avi_path
);
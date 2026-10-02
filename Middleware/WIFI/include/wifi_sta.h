#pragma once

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#define WIFI_STA_CONNECTED_BIT     BIT0
#define WIFI_STA_IPV4_OBTAINED_BIT BIT1

esp_err_t wifi_sta_init(EventGroupHandle_t event_group);
esp_err_t wifi_sta_stop(void);
#include "wifi_sta.h"

#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"

static const char *TAG = "wifi_sta";

static EventGroupHandle_t s_wifi_events = NULL;
static esp_netif_t *s_sta_netif = NULL;

static esp_event_handler_instance_t s_wifi_handler;
static esp_event_handler_instance_t s_ip_handler;

static bool s_started = false;
static bool s_stopping = false;

static void wifi_event_handler(void *arg,
                               esp_event_base_t event_base,
                               int32_t event_id,
                               void *event_data)
{
    (void)arg;

    if (event_base != WIFI_EVENT) {
        return;
    }

    switch (event_id) {
    case WIFI_EVENT_STA_START:
        ESP_LOGI(TAG, "Wi-Fi STA started");
        esp_wifi_connect();
        break;

    case WIFI_EVENT_STA_CONNECTED:
        ESP_LOGI(TAG, "Wi-Fi connected");

        if (s_wifi_events) {
            xEventGroupSetBits(
                s_wifi_events,
                WIFI_STA_CONNECTED_BIT
            );
        }
        break;

    case WIFI_EVENT_STA_DISCONNECTED: {
        wifi_event_sta_disconnected_t *disconnected =
            (wifi_event_sta_disconnected_t *)event_data;

        ESP_LOGW(TAG,
                 "Wi-Fi disconnected, reason=%d",
                 disconnected->reason);

        if (s_wifi_events) {
            xEventGroupClearBits(
                s_wifi_events,
                WIFI_STA_CONNECTED_BIT |
                WIFI_STA_IPV4_OBTAINED_BIT
            );
        }

#if CONFIG_WIFI_STA_AUTO_RECONNECT
        if (!s_stopping) {
            esp_err_t err = esp_wifi_connect();

            if (err != ESP_OK) {
                ESP_LOGW(TAG,
                         "Reconnect failed: %s",
                         esp_err_to_name(err));
            }
        }
#endif
        break;
    }

    default:
        break;
    }
}

static void ip_event_handler(void *arg,
                             esp_event_base_t event_base,
                             int32_t event_id,
                             void *event_data)
{
    (void)arg;
    (void)event_data;

    if (event_base == IP_EVENT &&
        event_id == IP_EVENT_STA_GOT_IP) {

        ESP_LOGI(TAG, "IPv4 address obtained");

        if (s_wifi_events) {
            xEventGroupSetBits(
                s_wifi_events,
                WIFI_STA_IPV4_OBTAINED_BIT
            );
        }
    }
}

esp_err_t wifi_sta_init(EventGroupHandle_t event_group)
{
#if !CONFIG_WIFI_STA_CONNECT
    ESP_LOGI(TAG, "Wi-Fi disabled by menuconfig");
    return ESP_OK;
#else

    if (s_started) {
        return ESP_OK;
    }

    s_wifi_events = event_group;
    s_stopping = false;

    s_sta_netif = esp_netif_create_default_wifi_sta();

    if (s_sta_netif == NULL) {
        ESP_LOGE(TAG, "Failed to create default Wi-Fi STA netif");
        return ESP_FAIL;
    }

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();

    esp_err_t err = esp_wifi_init(&init_cfg);

    if (err != ESP_OK) {
        ESP_LOGE(TAG,
                 "esp_wifi_init failed: %s",
                 esp_err_to_name(err));

        esp_netif_destroy_default_wifi(s_sta_netif);
        s_sta_netif = NULL;

        return err;
    }

    err = esp_event_handler_instance_register(
        WIFI_EVENT,
        ESP_EVENT_ANY_ID,
        &wifi_event_handler,
        NULL,
        &s_wifi_handler
    );

    if (err != ESP_OK) {
        goto fail;
    }

    err = esp_event_handler_instance_register(
        IP_EVENT,
        IP_EVENT_STA_GOT_IP,
        &ip_event_handler,
        NULL,
        &s_ip_handler
    );

    if (err != ESP_OK) {
        esp_event_handler_instance_unregister(
            WIFI_EVENT,
            ESP_EVENT_ANY_ID,
            s_wifi_handler
        );

        goto fail;
    }

    wifi_config_t wifi_config = {0};

    strncpy((char *)wifi_config.sta.ssid,
            CONFIG_WIFI_STA_SSID,
            sizeof(wifi_config.sta.ssid) - 1);

    strncpy((char *)wifi_config.sta.password,
            CONFIG_WIFI_STA_PASSWORD,
            sizeof(wifi_config.sta.password) - 1);

    wifi_config.sta.threshold.authmode =
        WIFI_AUTH_WPA2_PSK;

    err = esp_wifi_set_mode(WIFI_MODE_STA);

    if (err != ESP_OK) {
        goto fail_handlers;
    }

    err = esp_wifi_set_config(
        WIFI_IF_STA,
        &wifi_config
    );

    if (err != ESP_OK) {
        goto fail_handlers;
    }

    err = esp_wifi_start();

    if (err != ESP_OK) {
        goto fail_handlers;
    }

    s_started = true;

    ESP_LOGI(TAG, "Wi-Fi initialized");

    return ESP_OK;

fail_handlers:

    esp_event_handler_instance_unregister(
        IP_EVENT,
        IP_EVENT_STA_GOT_IP,
        s_ip_handler
    );

    esp_event_handler_instance_unregister(
        WIFI_EVENT,
        ESP_EVENT_ANY_ID,
        s_wifi_handler
    );

fail:

    esp_wifi_deinit();

    if (s_sta_netif) {
        esp_netif_destroy_default_wifi(s_sta_netif);
        s_sta_netif = NULL;
    }

    return err;

#endif
}

esp_err_t wifi_sta_stop(void)
{
#if !CONFIG_WIFI_STA_CONNECT
    return ESP_OK;
#else

    if (!s_started) {
        return ESP_OK;
    }

    s_stopping = true;

    if (s_wifi_events) {
        xEventGroupClearBits(
            s_wifi_events,
            WIFI_STA_CONNECTED_BIT |
            WIFI_STA_IPV4_OBTAINED_BIT
        );
    }

    esp_wifi_disconnect();
    esp_wifi_stop();

    esp_event_handler_instance_unregister(
        IP_EVENT,
        IP_EVENT_STA_GOT_IP,
        s_ip_handler
    );

    esp_event_handler_instance_unregister(
        WIFI_EVENT,
        ESP_EVENT_ANY_ID,
        s_wifi_handler
    );

    esp_wifi_deinit();

    if (s_sta_netif) {
        esp_netif_destroy_default_wifi(s_sta_netif);
        s_sta_netif = NULL;
    }

    s_started = false;
    s_wifi_events = NULL;

    return ESP_OK;

#endif
}
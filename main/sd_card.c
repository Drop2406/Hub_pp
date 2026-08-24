#include "sd_card.h"

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "driver/sdmmc_host.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"


#define SD_PIN_CLK 39
#define SD_PIN_CMD 38
#define SD_PIN_D0  40

#define SD_MAX_DIRECTORY_DEPTH 8

static const char *TAG = "SD_CARD";

static sdmmc_card_t *s_card = NULL;
static bool s_mounted = false;


bool sd_card_is_mounted(void)
{
    return s_mounted;
}


esp_err_t sd_card_mount(void)
{
    if (s_mounted) {
        ESP_LOGW(TAG, "SD card is already mounted");
        return ESP_OK;
    }

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 8,
        .allocation_unit_size = 16 * 1024,
    };

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();

    sdmmc_slot_config_t slot_config =
        SDMMC_SLOT_CONFIG_DEFAULT();

    slot_config.clk = SD_PIN_CLK;
    slot_config.cmd = SD_PIN_CMD;
    slot_config.d0 = SD_PIN_D0;

    /*
     * Chỉ dùng chế độ SDMMC 1-bit.
     */
    slot_config.width = 1;

    /*
     * Dùng pull-up nội của ESP32-S3.
     */
    slot_config.flags |=
        SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    ESP_LOGI(TAG, "Mounting SD card...");

    esp_err_t error = esp_vfs_fat_sdmmc_mount(
        SD_CARD_MOUNT_POINT,
        &host,
        &slot_config,
        &mount_config,
        &s_card
    );

    if (error != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Failed to mount SD card: %s",
            esp_err_to_name(error)
        );

        return error;
    }

    s_mounted = true;

    ESP_LOGI(
        TAG,
        "SD card mounted at %s",
        SD_CARD_MOUNT_POINT
    );

    sdmmc_card_print_info(
        stdout,
        s_card
    );

    return ESP_OK;
}


void sd_card_unmount(void)
{
    if (!s_mounted) {
        return;
    }

    esp_vfs_fat_sdcard_unmount(
        SD_CARD_MOUNT_POINT,
        s_card
    );

    s_card = NULL;
    s_mounted = false;

    ESP_LOGI(TAG, "SD card unmounted");
}


void sd_card_list_files(
    const char *directory,
    int level
)
{
    if (directory == NULL) {
        ESP_LOGE(TAG, "Directory path is NULL");
        return;
    }

    if (!s_mounted) {
        ESP_LOGE(TAG, "SD card is not mounted");
        return;
    }

    if (level > SD_MAX_DIRECTORY_DEPTH) {
        ESP_LOGW(
            TAG,
            "Maximum directory depth reached"
        );

        return;
    }

    DIR *dir = opendir(directory);

    if (dir == NULL) {
        ESP_LOGE(
            TAG,
            "Cannot open directory: %s",
            directory
        );

        return;
    }

    struct dirent *entry;
    struct stat entry_stat;

    char full_path[512];

    while ((entry = readdir(dir)) != NULL) {
        if (
            strcmp(entry->d_name, ".") == 0 ||
            strcmp(entry->d_name, "..") == 0
        ) {
            continue;
        }

        int written = snprintf(
            full_path,
            sizeof(full_path),
            "%s/%s",
            directory,
            entry->d_name
        );

        if (
            written < 0 ||
            written >= (int)sizeof(full_path)
        ) {
            ESP_LOGW(
                TAG,
                "Path too long: %s/%s",
                directory,
                entry->d_name
            );

            continue;
        }

        if (stat(full_path, &entry_stat) != 0) {
            ESP_LOGW(
                TAG,
                "Cannot stat: %s",
                full_path
            );

            continue;
        }

        char indent[64] = "";
        size_t indent_length = 0;

        for (
            int i = 0;
            i < level &&
            indent_length + 4 < sizeof(indent);
            i++
        ) {
            memcpy(
                &indent[indent_length],
                "   |",
                4
            );

            indent_length += 4;
            indent[indent_length] = '\0';
        }

        if (S_ISDIR(entry_stat.st_mode)) {
            ESP_LOGI(
                TAG,
                "%s[Directory] %s",
                indent,
                full_path
            );

            sd_card_list_files(
                full_path,
                level + 1
            );
        } else {
            ESP_LOGI(
                TAG,
                "%s[File] %s",
                indent,
                full_path
            );
        }
    }

    closedir(dir);
}
#include <stdbool.h>

#ifndef SD_CARD_H
#define SD_CARD_H

#include "esp_err.h"

#define SD_CARD_MOUNT_POINT "/sdcard"

esp_err_t sd_card_mount(void);

void sd_card_unmount(void);

void sd_card_list_files(
    const char *directory,
    int level
);

bool sd_card_is_mounted(void);

#endif
/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

#define APP_ASSETS_MOUNT_POINT "/assets"
#define APP_ASSETS_VIDEO_PATH APP_ASSETS_MOUNT_POINT "/video/video.bin"

esp_err_t app_assets_init(void);
bool app_assets_lock(TickType_t ticks_to_wait);
void app_assets_unlock(void);

bool app_assets_msc_ready(void);
uint32_t app_assets_msc_block_count(void);
uint16_t app_assets_msc_block_size(void);
esp_err_t app_assets_msc_read(uint32_t lba, uint32_t offset, void *buffer, uint32_t bufsize);
esp_err_t app_assets_msc_write(uint32_t lba, uint32_t offset, const void *buffer, uint32_t bufsize);

#ifdef __cplusplus
}
#endif

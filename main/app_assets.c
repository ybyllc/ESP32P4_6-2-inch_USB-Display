/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "app_assets.h"

#include <inttypes.h>
#include <string.h>
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "freertos/semphr.h"
#include "wear_levelling.h"

#define ASSETS_PARTITION_LABEL "assets"

static const char *TAG = "app_assets";
static wl_handle_t s_wl_handle = WL_INVALID_HANDLE;
static SemaphoreHandle_t s_assets_lock;
static bool s_assets_ready;

esp_err_t app_assets_init(void)
{
    if (s_assets_ready) {
        return ESP_OK;
    }

    if (s_assets_lock == NULL) {
        s_assets_lock = xSemaphoreCreateMutex();
        ESP_RETURN_ON_FALSE(s_assets_lock != NULL, ESP_ERR_NO_MEM, TAG, "assets lock alloc failed");
    }

    const esp_vfs_fat_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 4,
        .allocation_unit_size = 0,
        .disk_status_check_enable = false,
        .use_one_fat = false,
    };

    esp_err_t ret = esp_vfs_fat_spiflash_mount_rw_wl(APP_ASSETS_MOUNT_POINT,
                                                     ASSETS_PARTITION_LABEL,
                                                     &mount_config,
                                                     &s_wl_handle);
    ESP_RETURN_ON_ERROR(ret, TAG, "mount assets FAT partition failed");
    s_assets_ready = true;

    ESP_LOGI(TAG, "assets mounted at %s, blocks=%" PRIu32 ", block=%u",
             APP_ASSETS_MOUNT_POINT,
             app_assets_msc_block_count(),
             app_assets_msc_block_size());
    return ESP_OK;
}

bool app_assets_lock(TickType_t ticks_to_wait)
{
    return s_assets_lock != NULL && xSemaphoreTake(s_assets_lock, ticks_to_wait) == pdTRUE;
}

void app_assets_unlock(void)
{
    if (s_assets_lock) {
        xSemaphoreGive(s_assets_lock);
    }
}

bool app_assets_msc_ready(void)
{
    return s_assets_ready && s_wl_handle != WL_INVALID_HANDLE;
}

uint16_t app_assets_msc_block_size(void)
{
    if (s_wl_handle == WL_INVALID_HANDLE) {
        return 512;
    }
    return (uint16_t)wl_sector_size(s_wl_handle);
}

uint32_t app_assets_msc_block_count(void)
{
    if (s_wl_handle == WL_INVALID_HANDLE) {
        return 0;
    }
    const size_t block_size = wl_sector_size(s_wl_handle);
    return block_size ? (uint32_t)(wl_size(s_wl_handle) / block_size) : 0;
}

esp_err_t app_assets_msc_read(uint32_t lba, uint32_t offset, void *buffer, uint32_t bufsize)
{
    ESP_RETURN_ON_FALSE(buffer != NULL, ESP_ERR_INVALID_ARG, TAG, "read buffer is null");
    ESP_RETURN_ON_FALSE(app_assets_msc_ready(), ESP_ERR_INVALID_STATE, TAG, "assets MSC is not ready");

    const size_t block_size = wl_sector_size(s_wl_handle);
    const size_t addr = (size_t)lba * block_size + offset;
    ESP_RETURN_ON_FALSE(addr + bufsize <= wl_size(s_wl_handle), ESP_ERR_INVALID_SIZE, TAG, "MSC read out of range");

    ESP_RETURN_ON_FALSE(app_assets_lock(pdMS_TO_TICKS(1000)), ESP_ERR_TIMEOUT, TAG, "MSC read lock timeout");
    esp_err_t ret = wl_read(s_wl_handle, addr, buffer, bufsize);
    app_assets_unlock();
    return ret;
}

esp_err_t app_assets_msc_write(uint32_t lba, uint32_t offset, const void *buffer, uint32_t bufsize)
{
    ESP_RETURN_ON_FALSE(buffer != NULL, ESP_ERR_INVALID_ARG, TAG, "write buffer is null");
    ESP_RETURN_ON_FALSE(app_assets_msc_ready(), ESP_ERR_INVALID_STATE, TAG, "assets MSC is not ready");

    const size_t block_size = wl_sector_size(s_wl_handle);
    size_t addr = (size_t)lba * block_size + offset;
    const size_t end_addr = addr + bufsize;
    const uint8_t *src = (const uint8_t *)buffer;
    ESP_RETURN_ON_FALSE(offset < block_size, ESP_ERR_INVALID_SIZE, TAG, "MSC write offset out of range");
    ESP_RETURN_ON_FALSE(end_addr <= wl_size(s_wl_handle), ESP_ERR_INVALID_SIZE, TAG, "MSC write out of range");

    ESP_RETURN_ON_FALSE(app_assets_lock(pdMS_TO_TICKS(1000)), ESP_ERR_TIMEOUT, TAG, "MSC write lock timeout");
    uint8_t *sector_buf = heap_caps_malloc(block_size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!sector_buf) {
        app_assets_unlock();
        return ESP_ERR_NO_MEM;
    }

    esp_err_t ret = ESP_OK;
    while (addr < end_addr) {
        const size_t sector_addr = (addr / block_size) * block_size;
        const size_t sector_offset = addr - sector_addr;
        const size_t chunk = MIN(block_size - sector_offset, end_addr - addr);

        ret = wl_read(s_wl_handle, sector_addr, sector_buf, block_size);
        if (ret != ESP_OK) {
            break;
        }
        memcpy(sector_buf + sector_offset, src, chunk);

        ret = wl_erase_range(s_wl_handle, sector_addr, block_size);
        if (ret != ESP_OK) {
            break;
        }
        ret = wl_write(s_wl_handle, sector_addr, sector_buf, block_size);
        if (ret != ESP_OK) {
            break;
        }

        addr += chunk;
        src += chunk;
    }

    heap_caps_free(sector_buf);
    app_assets_unlock();
    return ret;
}

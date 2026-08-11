/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <inttypes.h>
#include <string.h>
#include "app_assets.h"
#include "esp_log.h"
#include "tusb.h"

static const char *TAG = "app_msc";

#if CFG_TUD_MSC

uint32_t tud_msc_inquiry2_cb(uint8_t lun, scsi_inquiry_resp_t *inquiry_resp, uint32_t bufsize)
{
    (void)lun;
    (void)bufsize;

    const char vid[] = "ESP32P4";
    const char pid[] = "Assets Disk";
    const char rev[] = "1.0";

    memcpy(inquiry_resp->vendor_id, vid, strlen(vid));
    memcpy(inquiry_resp->product_id, pid, strlen(pid));
    memcpy(inquiry_resp->product_rev, rev, strlen(rev));
    return sizeof(scsi_inquiry_resp_t);
}

bool tud_msc_test_unit_ready_cb(uint8_t lun)
{
    if (!app_assets_msc_ready()) {
        tud_msc_set_sense(lun, SCSI_SENSE_NOT_READY, 0x3a, 0x00);
        return false;
    }
    return true;
}

void tud_msc_capacity_cb(uint8_t lun, uint32_t *block_count, uint16_t *block_size)
{
    (void)lun;
    *block_count = app_assets_msc_block_count();
    *block_size = app_assets_msc_block_size();
}

int32_t tud_msc_read10_cb(uint8_t lun, uint32_t lba, uint32_t offset, void *buffer, uint32_t bufsize)
{
    (void)lun;
    esp_err_t ret = app_assets_msc_read(lba, offset, buffer, bufsize);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "MSC read failed: lba=%" PRIu32 " off=%" PRIu32 " size=%" PRIu32 " ret=%s",
                 lba, offset, bufsize, esp_err_to_name(ret));
        return -1;
    }
    return (int32_t)bufsize;
}

bool tud_msc_is_writable_cb(uint8_t lun)
{
    (void)lun;
    return false;
}

int32_t tud_msc_write10_cb(uint8_t lun, uint32_t lba, uint32_t offset, uint8_t *buffer, uint32_t bufsize)
{
    (void)lun;
    (void)lba;
    (void)offset;
    (void)buffer;
    (void)bufsize;
    ESP_LOGW(TAG, "MSC write rejected: USB disk is read-only");
    tud_msc_set_sense(lun, SCSI_SENSE_DATA_PROTECT, 0x27, 0x00);
    return -1;
}

bool tud_msc_start_stop_cb(uint8_t lun, uint8_t power_condition, bool start, bool load_eject)
{
    (void)lun;
    (void)power_condition;
    (void)start;
    (void)load_eject;
    return true;
}

int32_t tud_msc_scsi_cb(uint8_t lun, uint8_t const scsi_cmd[16], void *buffer, uint16_t bufsize)
{
    (void)buffer;
    (void)bufsize;

    switch (scsi_cmd[0]) {
    default:
        tud_msc_set_sense(lun, SCSI_SENSE_ILLEGAL_REQUEST, 0x20, 0x00);
        return -1;
    }
}

#endif

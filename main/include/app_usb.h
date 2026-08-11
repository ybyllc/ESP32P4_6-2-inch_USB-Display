/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdint.h>
#include "esp_err.h"
#include "tusb.h"
#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t press_down;
    uint8_t index;
    uint16_t x;
    uint16_t y;
    uint16_t width;
    uint16_t height;
} __attribute__((packed)) touch_report_t;

_Static_assert(CONFIG_ESP_LCD_TOUCH_MAX_POINTS == 5, "CONFIG_ESP_LCD_TOUCH_MAX_POINTS must be 5");

typedef struct {
    uint32_t report_id;    // Report identifier
    struct {
        touch_report_t data[CONFIG_ESP_LCD_TOUCH_MAX_POINTS];  // Touch report
        // uint16_t scan_time;
        uint8_t cnt;
    } touch_report;
} __attribute__((packed)) hid_report_t;

/**
 * @brief Initialize tinyusb device.
 *
 * @return
 *    - ESP_OK: Success
 *    - ESP_ERR_NO_MEM: No memory
 */
esp_err_t app_usb_init(void);

#if CFG_TUD_HID
/**
 * @brief Report key press in the keyboard, using array here
 *
 * @param report hid report data
 */
void tinyusb_hid_keyboard_report(hid_report_t report);

esp_err_t app_hid_init(void);
#endif

#if CFG_TUD_VENDOR
typedef struct {
    uint32_t rx_callbacks;
    uint32_t rx_bytes;
    uint32_t frames_started;
    uint32_t frames_completed;
    uint32_t frames_dropped_backpressure;
    uint32_t frames_dropped_no_empty;
    uint32_t stale_frames_skipped;
    uint32_t short_packets;
    uint32_t invalid_headers;
    uint32_t geometry_mismatch;
    uint32_t payload_errors;
    uint32_t queue_errors;
} app_vendor_stats_t;

esp_err_t app_vendor_init(void);
void app_vendor_get_stats(app_vendor_stats_t *stats);
void app_vendor_dump_stats(const char *reason);
void app_vendor_notify_usb_unmounted(void);
#endif

#ifdef __cplusplus
}
#endif

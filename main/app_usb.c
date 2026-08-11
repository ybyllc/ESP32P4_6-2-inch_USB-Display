/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <inttypes.h>
#include <stdint.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "esp_private/usb_phy.h"
#include "app_assets.h"
#include "usb_descriptors.h"
#include "device/usbd.h"
#include "usb_frame.h"
#include "app_usb.h"
#include "app_usb_health.h"

static const char *TAG = "app_usb";
static uint32_t s_usb_event_count = 0;
static int64_t s_last_usb_event_time = 0;

#if CONFIG_FREERTOS_NUMBER_OF_CORES > 1
#define USB_DEVICE_TASK_CORE 0
#else
#define USB_DEVICE_TASK_CORE tskNO_AFFINITY
#endif

#define USB_DEVICE_TASK_PRIORITY (((CONFIG_USB_TASK_PRIORITY + 2) < (configMAX_PRIORITIES - 1)) ? \
                                  (CONFIG_USB_TASK_PRIORITY + 2) : (configMAX_PRIORITIES - 1))

static void log_usb_event(const char *event, const char *detail)
{
    const int64_t now = esp_timer_get_time();
    const int64_t delta_ms = s_last_usb_event_time ? (now - s_last_usb_event_time) / 1000 : 0;

#if CFG_TUD_VENDOR
    app_vendor_stats_t stats = {0};
    app_vendor_get_stats(&stats);
    ESP_LOGI(TAG,
             "[%" PRIu32 "] %s at %" PRIi64 " ms (+%" PRIi64 " ms)%s%s | usb mounted=%d suspended=%d ready=%d | queue empty=%u filled=%u | vendor rx_cb=%" PRIu32 " rx_bytes=%" PRIu32 " started=%" PRIu32 " done=%" PRIu32 " drop_bp=%" PRIu32 " drop_no_empty=%" PRIu32 " stale=%" PRIu32 " short=%" PRIu32 " bad_hdr=%" PRIu32 " geom=%" PRIu32 " payload=%" PRIu32 " queue_err=%" PRIu32,
             ++s_usb_event_count,
             event,
             now / 1000,
             delta_ms,
             detail ? " | " : "",
             detail ? detail : "",
             tud_mounted(),
             tud_suspended(),
             tud_ready(),
             (unsigned)frame_empty_count(),
             (unsigned)frame_filled_count(),
             stats.rx_callbacks,
             stats.rx_bytes,
             stats.frames_started,
             stats.frames_completed,
             stats.frames_dropped_backpressure,
             stats.frames_dropped_no_empty,
             stats.stale_frames_skipped,
             stats.short_packets,
             stats.invalid_headers,
             stats.geometry_mismatch,
             stats.payload_errors,
             stats.queue_errors);
#else
    ESP_LOGI(TAG,
             "[%" PRIu32 "] %s at %" PRIi64 " ms (+%" PRIi64 " ms)%s%s | usb mounted=%d suspended=%d ready=%d",
             ++s_usb_event_count,
             event,
             now / 1000,
             delta_ms,
             detail ? " | " : "",
             detail ? detail : "",
             tud_mounted(),
             tud_suspended(),
             tud_ready());
#endif

    s_last_usb_event_time = now;
}

//--------------------------------------------------------------------+
// USB PHY config
//--------------------------------------------------------------------+
static void usb_phy_init(void)
{
    usb_phy_handle_t phy_hdl;
    // Configure USB PHY
    usb_phy_config_t phy_conf = {
        .controller = USB_PHY_CTRL_OTG,
        .otg_mode = USB_OTG_MODE_DEVICE,
    };
    phy_conf.target = USB_PHY_TARGET_INT;
    usb_new_phy(&phy_conf, &phy_hdl);
}

static void tusb_device_task(void *arg)
{
    while (1) {
        tud_task();
    }
}

esp_err_t app_usb_init(void)
{
    esp_err_t ret = ESP_OK;

    ret = app_assets_init();
    ESP_RETURN_ON_FALSE(ret == ESP_OK, ESP_FAIL, TAG, "app_assets_init failed");

    usb_phy_init();
    bool usb_init = tusb_init();
    if (!usb_init) {
        ESP_LOGE(TAG, "USB Device Stack Init Fail");
        return ESP_FAIL;
    }

    esp_err_t health_ret = app_usb_health_init();
    if (health_ret != ESP_OK) {
        ESP_LOGW(TAG, "USB health monitor init failed: %s", esp_err_to_name(health_ret));
    }

#if CFG_TUD_VENDOR
    ret = app_vendor_init();
    ESP_RETURN_ON_FALSE(ret == ESP_OK, ESP_FAIL, TAG, "app_vendor_init failed");
#endif

#if CFG_TUD_HID
    ret = app_hid_init();
    ESP_RETURN_ON_FALSE(ret == ESP_OK, ESP_FAIL, TAG, "app_hid_init failed");
#endif

    xTaskCreatePinnedToCore(tusb_device_task, "tusb_device_task", 4096, NULL, USB_DEVICE_TASK_PRIORITY, NULL, USB_DEVICE_TASK_CORE);
    return ret;
}

/************************************************** TinyUSB callbacks ***********************************************/
// Invoked when device is mounted
void tud_mount_cb(void)
{
    log_usb_event("USB Mount", NULL);
}

// Invoked when device is unmounted
void tud_umount_cb(void)
{
    log_usb_event("USB Un-Mount", NULL);
#if CFG_TUD_VENDOR
    app_vendor_notify_usb_unmounted();
#endif
}

// Invoked when usb bus is suspended
// remote_wakeup_en : if host allow us  to perform remote wakeup
// Within 7ms, device must draw an average of current less than 2.5 mA from bus
void tud_suspend_cb(bool remote_wakeup_en)
{
    char detail[32];
    snprintf(detail, sizeof(detail), "remote_wakeup=%d", remote_wakeup_en);
    log_usb_event("USB Suspend", detail);
}

// Invoked when usb bus is resumed
void tud_resume_cb(void)
{
    log_usb_event("USB Resume", NULL);
}

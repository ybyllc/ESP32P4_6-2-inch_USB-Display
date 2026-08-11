/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "app_usb_health.h"

#include <inttypes.h>
#include "app_usb.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "soc/usb_dwc_struct.h"
#include "tusb.h"

static const char *TAG = "usb_health";

#define USB_HEALTH_POLL_MS            20
#define USB_HEALTH_SOF_STALE_MS       100
#define USB_HEALTH_ABSENT_DEBOUNCE_MS 1500
#define USB_HEALTH_TASK_STACK         3072
#define USB_HEALTH_TASK_PRIORITY      (tskIDLE_PRIORITY + 2)

typedef enum {
    USB_HOST_ABSENT = 0,
    USB_HOST_PRESENT,
} usb_host_state_t;

static volatile usb_host_state_t s_host_state = USB_HOST_ABSENT;
static TaskHandle_t s_task_handle;

static void usb_health_task(void *arg)
{
    (void)arg;
    uint32_t last_soffn = 0;
    int64_t last_sof_ms = 0;
    bool ever_mounted = false;

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(USB_HEALTH_POLL_MS));

        const int64_t now_ms = esp_timer_get_time() / 1000;
        const uint32_t soffn = USB_DWC_HS.dsts_reg.soffn;
        const bool mounted = tud_mounted();
        const bool sof_alive = (soffn != last_soffn);
        last_soffn = soffn;

        if (sof_alive) {
            last_sof_ms = now_ms;
        }
        if (mounted) {
            ever_mounted = true;
        }

        bool present;
        if (sof_alive) {
            present = true;
        } else if (!mounted && !ever_mounted) {
            /* Never enumerated: plain power / charger / no host. */
            present = false;
        } else if ((now_ms - last_sof_ms) < USB_HEALTH_SOF_STALE_MS) {
            present = true;
        } else if (mounted && (now_ms - last_sof_ms) < USB_HEALTH_ABSENT_DEBOUNCE_MS) {
            /* Mounted but SOF paused (suspend or unplug): short grace period. */
            present = true;
        } else {
            present = false;
        }

        const usb_host_state_t new_state = present ? USB_HOST_PRESENT : USB_HOST_ABSENT;
        if (new_state != s_host_state) {
            ESP_LOGI(TAG, "USB host %s (mounted=%d sof_age=%" PRIi64 " ms)",
                     new_state == USB_HOST_PRESENT ? "present" : "lost",
                     mounted, now_ms - last_sof_ms);
            s_host_state = new_state;
            if (new_state == USB_HOST_ABSENT) {
                app_vendor_notify_usb_unmounted();
            }
        }
    }
}

bool app_usb_host_alive(void)
{
    return s_host_state == USB_HOST_PRESENT;
}

esp_err_t app_usb_health_init(void)
{
    ESP_RETURN_ON_FALSE(s_task_handle == NULL, ESP_ERR_INVALID_STATE, TAG, "usb health task already started");

    BaseType_t ok = xTaskCreatePinnedToCore(usb_health_task,
                                            "usb_health",
                                            USB_HEALTH_TASK_STACK,
                                            NULL,
                                            USB_HEALTH_TASK_PRIORITY,
                                            &s_task_handle,
                                            0);
    if (ok != pdPASS) {
        /* Fail open: keep the USB video path working. The umount callback
         * still handles real unmounts, but SOF-based idle restore is off. */
        s_host_state = USB_HOST_PRESENT;
        ESP_LOGE(TAG, "create usb health task failed");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <inttypes.h>
#include "esp_log.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_freertos_hooks.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "app_status_led.h"
#include "app_usb.h"
#if CONFIG_HID_TOUCH_ENABLE
#include "app_touch.h"
#endif
#include "app_lcd.h"

static const char *TAG = "usb_extend_screen";

#define SYSTEM_MONITOR_PERIOD_MS 2000

static volatile uint32_t s_idle_ticks[portNUM_PROCESSORS];

static bool cpu0_idle_hook(void)
{
    s_idle_ticks[0]++;
    return true;
}

#if portNUM_PROCESSORS > 1
static bool cpu1_idle_hook(void)
{
    s_idle_ticks[1]++;
    return true;
}
#endif

static uint32_t calc_busy_percent(uint32_t total_ticks, uint32_t idle_ticks)
{
    if (total_ticks == 0) {
        return 0;
    }

    if (idle_ticks >= total_ticks) {
        return 0;
    }

    return (uint32_t)(((uint64_t)(total_ticks - idle_ticks) * 100U) / total_ticks);
}

static void system_monitor_task(void *arg)
{
    uint32_t last_tick = xTaskGetTickCount();
    uint32_t last_idle_ticks[portNUM_PROCESSORS] = {0};

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(SYSTEM_MONITOR_PERIOD_MS));

        uint32_t now_tick = xTaskGetTickCount();
        uint32_t tick_delta = now_tick - last_tick;
        last_tick = now_tick;

        uint32_t idle_delta[portNUM_PROCESSORS] = {0};
        for (int core = 0; core < portNUM_PROCESSORS; core++) {
            uint32_t idle_now = s_idle_ticks[core];
            idle_delta[core] = idle_now - last_idle_ticks[core];
            last_idle_ticks[core] = idle_now;
        }

        const uint32_t cpu0_busy = calc_busy_percent(tick_delta, idle_delta[0]);
#if portNUM_PROCESSORS > 1
        const uint32_t cpu1_busy = calc_busy_percent(tick_delta, idle_delta[1]);
#else
        const uint32_t cpu1_busy = 0;
#endif

        const size_t ram_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        const size_t ram_min = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        const size_t ram_largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        const size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        const size_t psram_min = heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM);
        const size_t psram_largest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);

        ESP_LOGI(TAG,
                 "mon t=%" PRIi64 " ms | cpu busy: c0=%" PRIu32 "%% c1=%" PRIu32
                 "%% | ram free/min/maxblk=%u/%u/%u KB | psram free/min/maxblk=%u/%u/%u KB",
                 esp_timer_get_time() / 1000,
                 cpu0_busy,
                 cpu1_busy,
                 (unsigned)(ram_free / 1024),
                 (unsigned)(ram_min / 1024),
                 (unsigned)(ram_largest / 1024),
                 (unsigned)(psram_free / 1024),
                 (unsigned)(psram_min / 1024),
                 (unsigned)(psram_largest / 1024));
    }
}

static void start_system_monitor(void)
{
    ESP_ERROR_CHECK(esp_register_freertos_idle_hook_for_cpu(cpu0_idle_hook, 0));
#if portNUM_PROCESSORS > 1
    ESP_ERROR_CHECK(esp_register_freertos_idle_hook_for_cpu(cpu1_idle_hook, 1));
#endif

    BaseType_t ok = xTaskCreatePinnedToCore(system_monitor_task,
                                            "sys_mon",
                                            3072,
                                            NULL,
                                            tskIDLE_PRIORITY + 1,
                                            NULL,
                                            0);
    ESP_ERROR_CHECK(ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
}

void app_main(void)
{
    ESP_LOGI(TAG, "USB extend screen example");
    app_usb_init();
#if CONFIG_USB_EXTEND_SCREEN_USB_STABILITY_TEST_MODE
    ESP_LOGW(TAG, "USB stability test mode enabled: skip LCD/DSI init and rendering");
#else
    app_lcd_init();
#endif
#if CONFIG_HID_TOUCH_ENABLE
    app_touch_init();
#endif
    app_status_led_init();
    // start_system_monitor();
}

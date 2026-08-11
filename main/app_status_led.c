/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdbool.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "led_strip.h"
#include "app_lcd.h"
#include "app_usb.h"
#include "app_usb_health.h"

static const char *TAG = "app_status_led";

#define STATUS_LED_GPIO                  43
#define STATUS_LED_COUNT                 1
#define STATUS_LED_TASK_STACK            3072
#define STATUS_LED_TASK_PERIOD_MS        40
#define STATUS_LED_INACTIVE_TIMEOUT_MS   1200
#define STATUS_LED_BLINK_PERIOD_MS       500
#define STATUS_LED_BREATHE_PERIOD_MS     1800

#if CONFIG_FREERTOS_NUMBER_OF_CORES > 1
#define STATUS_LED_TASK_CORE             0
#else
#define STATUS_LED_TASK_CORE             tskNO_AFFINITY
#endif

#define STATUS_LED_TASK_PRIORITY         (tskIDLE_PRIORITY + 1)

typedef struct {
    uint8_t red;
    uint8_t green;
    uint8_t blue;
} rgb_t;

static led_strip_handle_t s_strip;

static uint8_t scale_u8(uint8_t value, uint16_t scale_255)
{
    return (uint8_t)((value * scale_255) / 255U);
}

static esp_err_t status_led_set_rgb(rgb_t color)
{
    ESP_RETURN_ON_FALSE(s_strip != NULL, ESP_ERR_INVALID_STATE, TAG, "strip not ready");
    ESP_RETURN_ON_ERROR(led_strip_set_pixel(s_strip, 0, color.red, color.green, color.blue), TAG, "set pixel failed");
    return led_strip_refresh(s_strip);
}

static rgb_t status_led_get_breathe_color(int64_t now_ms)
{
    const uint32_t phase_ms = (uint32_t)(now_ms % STATUS_LED_BREATHE_PERIOD_MS);
    const uint32_t half_period = STATUS_LED_BREATHE_PERIOD_MS / 2;
    uint16_t brightness = 0;

    if (phase_ms < half_period) {
        brightness = (uint16_t)((phase_ms * 255U) / half_period);
    } else {
        brightness = (uint16_t)(((STATUS_LED_BREATHE_PERIOD_MS - phase_ms) * 255U) / half_period);
    }

    brightness = 20U + (brightness * 180U) / 255U;

    return (rgb_t) {
        .red = 0,
        .green = scale_u8(90, brightness),
        .blue = scale_u8(180, brightness),
    };
}

static rgb_t status_led_get_blink_orange(int64_t now_ms)
{
    const bool led_on = ((now_ms / STATUS_LED_BLINK_PERIOD_MS) % 2) == 0;
    if (!led_on) {
        return (rgb_t) {0, 0, 0};
    }

    return (rgb_t) {
        .red = 96,
        .green = 28,
        .blue = 0,
    };
}

static rgb_t status_led_get_idle_color(void)
{
    return (rgb_t) {
        .red = 0,
        .green = 8,
        .blue = 18,
    };
}

static void status_led_task(void *arg)
{
    (void)arg;

    uint32_t last_flush_count = app_lcd_get_flush_count();
    int64_t last_active_ms = 0;
    rgb_t last_color = {255, 255, 255};

    while (1) {
        const int64_t now_ms = esp_timer_get_time() / 1000;
        const bool usb_connected = app_usb_host_alive();
        const uint32_t flush_count = app_lcd_get_flush_count();
        rgb_t color;

        if (flush_count != last_flush_count) {
            last_flush_count = flush_count;
            last_active_ms = now_ms;
        }

        if (!usb_connected) {
            color = status_led_get_blink_orange(now_ms);
        } else if ((now_ms - last_active_ms) <= STATUS_LED_INACTIVE_TIMEOUT_MS) {
            color = status_led_get_breathe_color(now_ms);
        } else {
            color = status_led_get_idle_color();
        }

        if (color.red != last_color.red || color.green != last_color.green || color.blue != last_color.blue) {
            esp_err_t ret = status_led_set_rgb(color);
            if (ret == ESP_OK) {
                last_color = color;
            } else {
                ESP_LOGW(TAG, "status led update failed: %s", esp_err_to_name(ret));
            }
        }

        vTaskDelay(pdMS_TO_TICKS(STATUS_LED_TASK_PERIOD_MS));
    }
}

esp_err_t app_status_led_init(void)
{
    led_strip_config_t strip_config = {
        .strip_gpio_num = STATUS_LED_GPIO,
        .max_leds = STATUS_LED_COUNT,
        .led_pixel_format = LED_PIXEL_FORMAT_GRB,
        .led_model = LED_MODEL_WS2812,
        .flags.invert_out = false,
    };
    led_strip_rmt_config_t rmt_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
        .mem_block_symbols = 64,
        .flags.with_dma = false,
    };

    esp_err_t ret = led_strip_new_rmt_device(&strip_config, &rmt_config, &s_strip);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "status led init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_RETURN_ON_ERROR(led_strip_clear(s_strip), TAG, "clear status led failed");

    BaseType_t ok = xTaskCreatePinnedToCore(status_led_task,
                                            "status_led",
                                            STATUS_LED_TASK_STACK,
                                            NULL,
                                            STATUS_LED_TASK_PRIORITY,
                                            NULL,
                                            STATUS_LED_TASK_CORE);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "create status led task failed");

    ESP_LOGI(TAG, "WS2812 status LED ready on GPIO %d", STATUS_LED_GPIO);
    return ESP_OK;
}

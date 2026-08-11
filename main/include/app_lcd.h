/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESP_LCD_H
#define ESP_LCD_H

#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_err.h"
#include "esp_lcd_panel_ops.h"
#include "usb_frame.h"

#ifdef __cplusplus
extern "C" {
#endif

#define EXAMPLE_LCD_H_RES                   (CONFIG_USB_EXTEND_SCREEN_HEIGHT)
#define EXAMPLE_LCD_V_RES                   (CONFIG_USB_EXTEND_SCREEN_WIDTH)

#define EXAMPLE_LCD_BUF_NUM                 (CONFIG_EXAMPLE_LCD_BUF_COUNT)

#if CONFIG_LCD_PIXEL_FORMAT_RGB565
#define EXAMPLE_LCD_BIT_PER_PIXEL           (16)
#elif CONFIG_LCD_PIXEL_FORMAT_RGB888
#define EXAMPLE_LCD_BIT_PER_PIXEL           (24)
#endif

#define EXAMPLE_LCD_BUF_LEN                 EXAMPLE_LCD_H_RES * EXAMPLE_LCD_V_RES * EXAMPLE_LCD_BIT_PER_PIXEL / 8

/**
 * @brief Initialize the LCD panel.
 *
 * This function initializes the LCD panel with the provided panel handle. It powers on the LCD,
 * installs the LCD driver, configures the bus, and sets up the panel.
 *
 * @return
 *    - ESP_OK: Success
 *    - ESP_FAIL: Failure
 */
esp_err_t app_lcd_init(void);

esp_err_t app_lcd_submit_frame(frame_t *frame);

esp_err_t app_lcd_prepare_stream_mode(void);

bool app_lcd_should_drop_frame(void);

uint32_t app_lcd_get_flush_count(void);

void app_lcd_notify_video_stream_start(void);

void app_lcd_notify_video_stream_lost(void);

#ifdef __cplusplus
}
#endif

#endif

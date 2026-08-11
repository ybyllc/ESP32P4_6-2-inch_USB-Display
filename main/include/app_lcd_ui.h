/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_lcd_panel_ops.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    APP_LCD_UI_PANEL_DSI = 0,
} app_lcd_ui_panel_type_t;

typedef struct {
    app_lcd_ui_panel_type_t panel_type;
    esp_lcd_panel_handle_t panel_handle;
    esp_lcd_panel_handle_t control_handle;
    uint32_t hres;
    uint32_t vres;
    lv_color_format_t color_format;
} app_lcd_ui_config_t;

esp_err_t app_lcd_ui_init(const app_lcd_ui_config_t *config);

void app_lcd_ui_notify_video_stream(void);

bool app_lcd_ui_cancel_pending_stream(void);

bool app_lcd_ui_video_output_enabled(void);

#ifdef __cplusplus
}
#endif

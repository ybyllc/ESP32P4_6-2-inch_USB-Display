/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

extern volatile bool g_hid_touch_filter_enabled;

esp_err_t app_touch_init(void);
esp_err_t app_touch_bind_lvgl(lv_display_t *display);
void app_touch_unbind_lvgl(void);
void app_touch_route_to_hid(void);
void app_touch_filter_set_enabled(bool enabled);
bool app_touch_filter_get_enabled(void);
void app_touch_hid_set_enabled(bool enabled);
bool app_touch_hid_get_enabled(void);

#ifdef __cplusplus
}
#endif

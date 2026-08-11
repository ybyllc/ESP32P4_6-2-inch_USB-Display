/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_lvgl_port.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "app_lcd_ui.h"
#include "app_usb.h"
#include "app_usb_health.h"
#if CONFIG_HID_TOUCH_ENABLE
#include "app_touch.h"
#endif

#include "esp_lcd_mipi_dsi.h"

LV_FONT_DECLARE(simhei_50);
LV_FONT_DECLARE(simhei_30);

static const char *TAG = "app_lcd_ui";

#define LCD_UI_TASK_STACK_SIZE 4096
#define LCD_UI_TASK_PRIORITY (tskIDLE_PRIORITY + 2)
#define LCD_UI_TASK_PERIOD_MS 100
#define LCD_UI_COUNTDOWN_MS 3000
#define LCD_UI_DRAW_BUFFER_LINES 64
#define LCD_UI_STATUS_TEXT_MAX_LEN 64
#define LCD_UI_LVGL_INIT_WAIT_MS 1000
#define LCD_UI_PANEL_OPA 150
#define LCD_UI_MAIN_PANEL_W 320
#define LCD_UI_MAIN_PANEL_H 320
#define LCD_UI_USB_PANEL_W 320
#define LCD_UI_USB_PANEL_H 80
#define LCD_UI_PANEL_GAP 10
#define LCD_UI_SETTINGS_BTN_SIZE 44
#define LCD_UI_FLOATING_SETTINGS_BTN_SIZE 56
#define LCD_UI_NVS_NAMESPACE "ui_cfg"
#define LCD_UI_NVS_KEY_PAGE_HIDE "page_hide"
#define LCD_UI_NVS_KEY_HID_TOUCH "hid_touch"

typedef struct
{
    app_lcd_ui_config_t config;
    lv_display_t *display;
    lv_obj_t *usb_panel;
    lv_obj_t *main_panel;
    lv_obj_t *usb_label;
    lv_obj_t *status_label;
    lv_obj_t *countdown_label;
    lv_obj_t *countdown_arc;
    lv_obj_t *spinner;
    lv_obj_t *settings_icon;
    lv_obj_t *floating_settings_icon;
    lv_obj_t *settings_page;
    lv_obj_t *back_icon;
    lv_obj_t *hide_switch;
    lv_obj_t *hid_switch;
    TaskHandle_t task_handle;
    volatile bool stream_detected;
    volatile bool video_output_enabled;
    bool lvgl_ready;
    bool countdown_started;
    bool settings_open;
    bool page_hidden;
    bool hid_touch_enabled;
    bool nvs_ready;
    int64_t countdown_start_ms;
    char last_status[LCD_UI_STATUS_TEXT_MAX_LEN];
} app_lcd_ui_ctx_t;

static app_lcd_ui_ctx_t s_ui_ctx;

static void app_lcd_ui_task(void *arg);
static void app_lcd_ui_build_screen_locked(void);
static void app_lcd_ui_update_waiting_status_locked(void);
static void app_lcd_ui_update_countdown_locked(int seconds_left);
static void app_lcd_ui_shutdown_lvgl(void);
static esp_err_t app_lcd_ui_wait_lvgl_ready(void);
static void app_lcd_ui_start_intro_anim(lv_obj_t *obj, uint32_t delay);
static void app_lcd_ui_start_countdown_anim_locked(void);
static esp_err_t app_lcd_ui_settings_load(void);
static void app_lcd_ui_settings_save_bool(const char *key, bool value);
static void app_lcd_ui_apply_visibility_locked(void);
static void app_lcd_ui_open_settings_locked(void);
static void app_lcd_ui_close_settings_locked(void);

#include "video_player.h"

static lv_obj_t *s_video_bg = NULL;
static bool s_video_started = false;
static bool s_countdown_anim_started = false;
static SemaphoreHandle_t s_ui_lock;
static bool s_ui_shutting_down;

static void app_lcd_ui_anim_set_opa(void *obj, int32_t value)
{
    lv_obj_set_style_opa((lv_obj_t *)obj, (lv_opa_t)value, 0);
}

static void app_lcd_ui_anim_set_translate_y(void *obj, int32_t value)
{
    lv_obj_set_style_translate_y((lv_obj_t *)obj, value, 0);
}

static void app_lcd_ui_anim_set_arc_value(void *obj, int32_t value)
{
    lv_arc_set_value((lv_obj_t *)obj, value);
}

static void app_lcd_ui_style_panel(lv_obj_t *panel, int32_t width, int32_t height)
{
    lv_obj_remove_style_all(panel);
    lv_obj_set_size(panel, width, height);
    lv_obj_set_style_bg_color(panel, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(panel, LCD_UI_PANEL_OPA, 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_border_color(panel, lv_color_white(), 0);
    lv_obj_set_style_border_opa(panel, 70, 0);
    lv_obj_set_style_radius(panel, 18, 0);
    lv_obj_set_style_pad_all(panel, 0, 0);
    lv_obj_set_style_shadow_color(panel, lv_color_black(), 0);
    lv_obj_set_style_shadow_opa(panel, 70, 0);
    lv_obj_set_style_shadow_width(panel, 24, 0);
    lv_obj_set_style_shadow_offset_y(panel, 8, 0);
    lv_obj_set_scrollbar_mode(panel, LV_SCROLLBAR_MODE_OFF);
}

static void app_lcd_ui_set_hidden(lv_obj_t *obj, bool hidden)
{
    if (obj == NULL) {
        return;
    }

    if (hidden) {
        lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
    }
}

static void app_lcd_ui_style_icon_button(lv_obj_t *btn, int32_t size, lv_opa_t bg_opa)
{
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, size, size);
    lv_obj_set_style_radius(btn, size / 2, 0);
    lv_obj_set_style_bg_color(btn, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(btn, bg_opa, 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_border_color(btn, lv_color_white(), 0);
    lv_obj_set_style_border_opa(btn, 70, 0);
    lv_obj_set_style_shadow_color(btn, lv_color_black(), 0);
    lv_obj_set_style_shadow_opa(btn, 55, 0);
    lv_obj_set_style_shadow_width(btn, 14, 0);
    lv_obj_set_style_shadow_offset_y(btn, 4, 0);
    lv_obj_set_style_pad_all(btn, 0, 0);
    lv_obj_set_scrollbar_mode(btn, LV_SCROLLBAR_MODE_OFF);
}

static lv_obj_t *app_lcd_ui_create_icon_button(lv_obj_t *parent, const char *symbol, int32_t size, lv_opa_t bg_opa)
{
    lv_obj_t *btn = lv_button_create(parent);
    app_lcd_ui_style_icon_button(btn, size, bg_opa);

    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text(label, symbol);
    lv_obj_set_style_text_color(label, lv_color_hex(0x203040), 0);
    lv_obj_center(label);
    return btn;
}

static void app_lcd_ui_settings_save_bool(const char *key, bool value)
{
    if (!s_ui_ctx.nvs_ready) {
        return;
    }

    nvs_handle_t handle = 0;
    esp_err_t ret = nvs_open(LCD_UI_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "open NVS failed: %s", esp_err_to_name(ret));
        return;
    }

    ret = nvs_set_u8(handle, key, value ? 1 : 0);
    if (ret == ESP_OK) {
        ret = nvs_commit(handle);
    }
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "save %s failed: %s", key, esp_err_to_name(ret));
    }
    nvs_close(handle);
}

static esp_err_t app_lcd_ui_settings_load(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "NVS erase failed");
        ret = nvs_flash_init();
    }
    ESP_RETURN_ON_ERROR(ret, TAG, "NVS init failed");

    s_ui_ctx.nvs_ready = true;
    nvs_handle_t handle = 0;
    ret = nvs_open(LCD_UI_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(ret, TAG, "open NVS failed");

    uint8_t value = 0;
    if (nvs_get_u8(handle, LCD_UI_NVS_KEY_PAGE_HIDE, &value) == ESP_OK) {
        s_ui_ctx.page_hidden = (value != 0);
    }
    value = 1;
    if (nvs_get_u8(handle, LCD_UI_NVS_KEY_HID_TOUCH, &value) == ESP_OK) {
        s_ui_ctx.hid_touch_enabled = (value != 0);
    }
    nvs_close(handle);
    return ESP_OK;
}

static void app_lcd_ui_apply_visibility_locked(void)
{
    if (s_ui_ctx.settings_open) {
        app_lcd_ui_set_hidden(s_ui_ctx.main_panel, false);
        app_lcd_ui_set_hidden(s_ui_ctx.usb_panel, true);
        app_lcd_ui_set_hidden(s_ui_ctx.floating_settings_icon, true);
        app_lcd_ui_set_hidden(s_ui_ctx.spinner, true);
        app_lcd_ui_set_hidden(s_ui_ctx.countdown_arc, true);
        app_lcd_ui_set_hidden(s_ui_ctx.countdown_label, true);
        app_lcd_ui_set_hidden(s_ui_ctx.settings_icon, true);
        app_lcd_ui_set_hidden(s_ui_ctx.settings_page, false);
        return;
    }

    app_lcd_ui_set_hidden(s_ui_ctx.settings_page, true);
    app_lcd_ui_set_hidden(s_ui_ctx.settings_icon, false);

    if (s_ui_ctx.page_hidden) {
        app_lcd_ui_set_hidden(s_ui_ctx.main_panel, true);
        app_lcd_ui_set_hidden(s_ui_ctx.usb_panel, true);
        app_lcd_ui_set_hidden(s_ui_ctx.floating_settings_icon, false);
    } else {
        app_lcd_ui_set_hidden(s_ui_ctx.main_panel, false);
        app_lcd_ui_set_hidden(s_ui_ctx.usb_panel, false);
        app_lcd_ui_set_hidden(s_ui_ctx.floating_settings_icon, true);
    }
}

static void app_lcd_ui_open_settings_locked(void)
{
    s_ui_ctx.settings_open = true;
    app_lcd_ui_apply_visibility_locked();
    app_lcd_ui_start_intro_anim(s_ui_ctx.settings_page, 0);
}

static void app_lcd_ui_close_settings_locked(void)
{
    s_ui_ctx.settings_open = false;
    app_lcd_ui_apply_visibility_locked();
    if (!s_ui_ctx.page_hidden && !s_ui_ctx.stream_detected) {
        app_lcd_ui_update_waiting_status_locked();
    }
}

static void app_lcd_ui_settings_icon_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) == LV_EVENT_CLICKED) {
        app_lcd_ui_open_settings_locked();
    }
}

static void app_lcd_ui_back_icon_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) == LV_EVENT_CLICKED) {
        app_lcd_ui_close_settings_locked();
    }
}

static void app_lcd_ui_switch_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_VALUE_CHANGED) {
        return;
    }

    lv_obj_t *target = lv_event_get_target_obj(event);
    const bool enabled = lv_obj_has_state(target, LV_STATE_CHECKED);
    if (target == s_ui_ctx.hide_switch) {
        s_ui_ctx.page_hidden = enabled;
        app_lcd_ui_settings_save_bool(LCD_UI_NVS_KEY_PAGE_HIDE, enabled);
    } else if (target == s_ui_ctx.hid_switch) {
        s_ui_ctx.hid_touch_enabled = enabled;
        app_lcd_ui_settings_save_bool(LCD_UI_NVS_KEY_HID_TOUCH, enabled);
#if CONFIG_HID_TOUCH_ENABLE
        app_touch_hid_set_enabled(enabled);
#endif
    }
}

static lv_obj_t *app_lcd_ui_create_setting_label(lv_obj_t *parent, const char *text, int32_t y)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_width(label, 170);
    lv_obj_set_style_text_font(label, &simhei_30, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(0x16202C), 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_pos(label, 34, y);
    return label;
}

static lv_obj_t *app_lcd_ui_create_setting_switch(lv_obj_t *parent, bool checked, int32_t y)
{
    lv_obj_t *sw = lv_switch_create(parent);
    lv_obj_set_size(sw, 64, 34);
    lv_obj_set_pos(sw, 220, y - 2);
    lv_obj_set_style_bg_color(sw, lv_color_hex(0xD7DFE8), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(sw, 170, LV_PART_MAIN);
    lv_obj_set_style_bg_color(sw, lv_color_hex(0xF28C28), LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(sw, lv_color_white(), LV_PART_KNOB);
    if (checked) {
        lv_obj_add_state(sw, LV_STATE_CHECKED);
    }
    lv_obj_add_event_cb(sw, app_lcd_ui_switch_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
    return sw;
}

static void app_lcd_ui_create_settings_page_locked(void)
{
    s_ui_ctx.settings_page = lv_obj_create(s_ui_ctx.main_panel);
    lv_obj_remove_style_all(s_ui_ctx.settings_page);
    lv_obj_set_size(s_ui_ctx.settings_page, LCD_UI_MAIN_PANEL_W, LCD_UI_MAIN_PANEL_H);
    lv_obj_set_pos(s_ui_ctx.settings_page, 0, 0);
    lv_obj_set_style_bg_opa(s_ui_ctx.settings_page, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(s_ui_ctx.settings_page, 0, 0);
    lv_obj_set_scrollbar_mode(s_ui_ctx.settings_page, LV_SCROLLBAR_MODE_OFF);

    s_ui_ctx.back_icon = app_lcd_ui_create_icon_button(s_ui_ctx.settings_page, LV_SYMBOL_LEFT, LCD_UI_SETTINGS_BTN_SIZE, 110);
    lv_obj_set_pos(s_ui_ctx.back_icon, 14, 14);
    lv_obj_add_event_cb(s_ui_ctx.back_icon, app_lcd_ui_back_icon_event_cb, LV_EVENT_CLICKED, NULL);

    app_lcd_ui_create_setting_label(s_ui_ctx.settings_page, "页面隐藏", 105);
    s_ui_ctx.hide_switch = app_lcd_ui_create_setting_switch(s_ui_ctx.settings_page, s_ui_ctx.page_hidden, 104);

    app_lcd_ui_create_setting_label(s_ui_ctx.settings_page, "HID触摸", 174);
    s_ui_ctx.hid_switch = app_lcd_ui_create_setting_switch(s_ui_ctx.settings_page, s_ui_ctx.hid_touch_enabled, 173);

    lv_obj_t *version_label = lv_label_create(s_ui_ctx.settings_page);
    lv_label_set_text(version_label, "version v1.0.1");
    lv_obj_set_width(version_label, LCD_UI_MAIN_PANEL_W - 24);
    lv_obj_set_style_text_align(version_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(version_label, &simhei_30, 0);
    lv_obj_set_style_text_color(version_label, lv_color_hex(0x8A94A6), 0);
    lv_obj_align(version_label, LV_ALIGN_BOTTOM_MID, 0, -12);

    lv_obj_add_flag(s_ui_ctx.settings_page, LV_OBJ_FLAG_HIDDEN);
}

static void app_lcd_ui_start_intro_anim(lv_obj_t *obj, uint32_t delay)
{
    lv_obj_set_style_opa(obj, 0, 0);
    lv_obj_set_style_translate_y(obj, 12, 0);

    lv_anim_t fade;
    lv_anim_init(&fade);
    lv_anim_set_var(&fade, obj);
    lv_anim_set_values(&fade, 0, LV_OPA_COVER);
    lv_anim_set_duration(&fade, 420);
    lv_anim_set_delay(&fade, delay);
    lv_anim_set_path_cb(&fade, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&fade, app_lcd_ui_anim_set_opa);
    lv_anim_start(&fade);

    lv_anim_t slide;
    lv_anim_init(&slide);
    lv_anim_set_var(&slide, obj);
    lv_anim_set_values(&slide, 12, 0);
    lv_anim_set_duration(&slide, 520);
    lv_anim_set_delay(&slide, delay);
    lv_anim_set_path_cb(&slide, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&slide, app_lcd_ui_anim_set_translate_y);
    lv_anim_start(&slide);
}

static void app_lcd_ui_start_countdown_anim_locked(void)
{
    if (s_countdown_anim_started || s_ui_ctx.countdown_arc == NULL) {
        return;
    }

    s_countdown_anim_started = true;
    lv_arc_set_value(s_ui_ctx.countdown_arc, 0);

    lv_anim_t arc_anim;
    lv_anim_init(&arc_anim);
    lv_anim_set_var(&arc_anim, s_ui_ctx.countdown_arc);
    lv_anim_set_values(&arc_anim, 0, 100);
    lv_anim_set_duration(&arc_anim, LCD_UI_COUNTDOWN_MS);
    lv_anim_set_path_cb(&arc_anim, lv_anim_path_ease_in_out);
    lv_anim_set_exec_cb(&arc_anim, app_lcd_ui_anim_set_arc_value);
    lv_anim_start(&arc_anim);
}

static void app_lcd_ui_build_screen_locked(void)
{
    lv_obj_t *screen = lv_screen_active();

    lv_obj_clean(screen);
    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(screen, lv_color_hex(0x203040), 0);
    lv_obj_set_style_pad_all(screen, 0, 0);

    if (s_video_bg == NULL)
    {
        lv_display_t *disp = lv_display_get_default();
        int32_t width = lv_display_get_horizontal_resolution(disp);
        int32_t height = lv_display_get_vertical_resolution(disp);

        s_video_bg = lv_image_create(screen);
        lv_obj_set_size(s_video_bg, width, height);
        lv_obj_set_pos(s_video_bg, 0, 0);
        lv_obj_move_background(s_video_bg);
    }
    if (!s_video_started)
    {
        if (video_player_start(s_video_bg) == ESP_OK)
        {
            s_video_started = true;
        }
    }

    s_ui_ctx.main_panel = lv_obj_create(screen);
    app_lcd_ui_style_panel(s_ui_ctx.main_panel, LCD_UI_MAIN_PANEL_W, LCD_UI_MAIN_PANEL_H);
    lv_obj_align(s_ui_ctx.main_panel, LV_ALIGN_CENTER, 0, 0);

    s_ui_ctx.usb_panel = lv_obj_create(screen);
    app_lcd_ui_style_panel(s_ui_ctx.usb_panel, LCD_UI_USB_PANEL_W, LCD_UI_USB_PANEL_H);
    lv_obj_align_to(s_ui_ctx.usb_panel, s_ui_ctx.main_panel, LV_ALIGN_OUT_TOP_MID, 0, -LCD_UI_PANEL_GAP);

    s_ui_ctx.usb_label = lv_label_create(s_ui_ctx.usb_panel);
    lv_obj_set_width(s_ui_ctx.usb_label, LCD_UI_USB_PANEL_W - 32);
    lv_obj_set_style_text_align(s_ui_ctx.usb_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(s_ui_ctx.usb_label, &simhei_50, 0);
    lv_obj_set_style_text_color(s_ui_ctx.usb_label, lv_color_hex(0x102033), 0);
    lv_obj_set_style_text_opa(s_ui_ctx.usb_label, LV_OPA_COVER, 0);
    lv_obj_center(s_ui_ctx.usb_label);

    s_ui_ctx.settings_icon = app_lcd_ui_create_icon_button(s_ui_ctx.main_panel, LV_SYMBOL_SETTINGS, LCD_UI_SETTINGS_BTN_SIZE, 150);
    lv_obj_align(s_ui_ctx.settings_icon, LV_ALIGN_TOP_RIGHT, -12, 12);
    lv_obj_add_event_cb(s_ui_ctx.settings_icon, app_lcd_ui_settings_icon_event_cb, LV_EVENT_CLICKED, NULL);

    s_ui_ctx.floating_settings_icon = app_lcd_ui_create_icon_button(screen, LV_SYMBOL_SETTINGS, LCD_UI_FLOATING_SETTINGS_BTN_SIZE, 150);
    lv_obj_align(s_ui_ctx.floating_settings_icon, LV_ALIGN_TOP_LEFT, 12, 12);
    lv_obj_add_event_cb(s_ui_ctx.floating_settings_icon, app_lcd_ui_settings_icon_event_cb, LV_EVENT_CLICKED, NULL);

    const int32_t spinner_size = (int32_t)((s_ui_ctx.config.hres < s_ui_ctx.config.vres ? s_ui_ctx.config.hres : s_ui_ctx.config.vres) / 2U);
    s_ui_ctx.spinner = lv_spinner_create(s_ui_ctx.main_panel);
    // lv_spinner_set_anim_duration(s_ui_ctx.spinner, 900);
    // lv_spinner_set_arc_sweep(s_ui_ctx.spinner, 110);
    lv_obj_set_size(s_ui_ctx.spinner, spinner_size, spinner_size);
    lv_obj_set_style_bg_opa(s_ui_ctx.spinner, LV_OPA_TRANSP, 0);
    lv_obj_set_style_arc_width(s_ui_ctx.spinner, 12, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_ui_ctx.spinner, lv_color_hex(0xF28C28), LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(s_ui_ctx.spinner, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(s_ui_ctx.spinner, 12, LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_ui_ctx.spinner, lv_color_hex(0xF9D5AE), LV_PART_MAIN);
    lv_obj_set_style_arc_opa(s_ui_ctx.spinner, 190, LV_PART_MAIN);
    lv_obj_align(s_ui_ctx.spinner, LV_ALIGN_CENTER, 0, 0);

    s_ui_ctx.status_label = lv_label_create(s_ui_ctx.main_panel);
    lv_obj_add_flag(s_ui_ctx.status_label, LV_OBJ_FLAG_HIDDEN);

    s_ui_ctx.countdown_arc = lv_arc_create(s_ui_ctx.main_panel);
    lv_obj_set_size(s_ui_ctx.countdown_arc, 152, 152);
    lv_arc_set_range(s_ui_ctx.countdown_arc, 0, 100);
    lv_arc_set_value(s_ui_ctx.countdown_arc, 0);
    lv_arc_set_rotation(s_ui_ctx.countdown_arc, 270);
    lv_arc_set_bg_angles(s_ui_ctx.countdown_arc, 0, 360);
    lv_obj_remove_flag(s_ui_ctx.countdown_arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(s_ui_ctx.countdown_arc, 11, LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_ui_ctx.countdown_arc, lv_color_hex(0xD7E4F2), LV_PART_MAIN);
    lv_obj_set_style_arc_opa(s_ui_ctx.countdown_arc, 190, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_ui_ctx.countdown_arc, 11, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_ui_ctx.countdown_arc, lv_color_hex(0x2677E8), LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(s_ui_ctx.countdown_arc, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_align(s_ui_ctx.countdown_arc, LV_ALIGN_CENTER, 0, -24);
    lv_obj_add_flag(s_ui_ctx.countdown_arc, LV_OBJ_FLAG_HIDDEN);

    s_ui_ctx.countdown_label = lv_label_create(s_ui_ctx.main_panel);
    lv_obj_set_width(s_ui_ctx.countdown_label, LCD_UI_MAIN_PANEL_W - 40);
    lv_obj_set_style_text_align(s_ui_ctx.countdown_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(s_ui_ctx.countdown_label, &simhei_50, 0);
    lv_obj_set_style_text_color(s_ui_ctx.countdown_label, lv_color_hex(0x102033), 0);
    lv_obj_align(s_ui_ctx.countdown_label, LV_ALIGN_CENTER, 0, -24);
    lv_label_set_text(s_ui_ctx.countdown_label, "");
    lv_obj_add_flag(s_ui_ctx.countdown_label, LV_OBJ_FLAG_HIDDEN);

    app_lcd_ui_create_settings_page_locked();

    app_lcd_ui_start_intro_anim(s_ui_ctx.usb_panel, 80);
    app_lcd_ui_start_intro_anim(s_ui_ctx.main_panel, 180);

    s_ui_ctx.last_status[0] = '\0';
    s_countdown_anim_started = false;
    app_lcd_ui_update_waiting_status_locked();
    app_lcd_ui_apply_visibility_locked();
}

static void app_lcd_ui_update_waiting_status_locked(void)
{
    if (s_ui_ctx.settings_open) {
        return;
    }

    const bool usb_ready = app_usb_host_alive();
    const char *status = usb_ready ? "USB已连接" : "USB未连接";

    if (s_ui_ctx.spinner) {
        lv_obj_remove_flag(s_ui_ctx.spinner, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_ui_ctx.countdown_arc) {
        lv_obj_add_flag(s_ui_ctx.countdown_arc, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_ui_ctx.countdown_label) {
        lv_obj_add_flag(s_ui_ctx.countdown_label, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(s_ui_ctx.countdown_label, "");
    }

    if (strncmp(s_ui_ctx.last_status, status, sizeof(s_ui_ctx.last_status)) == 0)
    {
        return;
    }

    snprintf(s_ui_ctx.last_status, sizeof(s_ui_ctx.last_status), "%s", status);
    lv_label_set_text(s_ui_ctx.usb_label, status);
    app_lcd_ui_apply_visibility_locked();
}

static void app_lcd_ui_update_countdown_locked(int seconds_left)
{
    if (s_ui_ctx.settings_open) {
        return;
    }

    lv_label_set_text(s_ui_ctx.usb_label, "USB已连接");
    lv_obj_add_flag(s_ui_ctx.spinner, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_ui_ctx.countdown_arc, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_ui_ctx.countdown_label, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text_fmt(s_ui_ctx.countdown_label, "%d", seconds_left);
    app_lcd_ui_start_countdown_anim_locked();
    snprintf(s_ui_ctx.last_status, sizeof(s_ui_ctx.last_status), "%s", "USB已连接");
    app_lcd_ui_apply_visibility_locked();
}

static void app_lcd_ui_shutdown_lvgl(void)
{
    if (s_video_started)
    {
        video_player_stop();
        s_video_started = false;
    }

    if (!s_ui_ctx.lvgl_ready)
    {
        s_ui_ctx.video_output_enabled = true;
        return;
    }

    if (s_ui_ctx.config.panel_type == APP_LCD_UI_PANEL_DSI)
    {
        const esp_lcd_dpi_panel_event_callbacks_t callbacks = {0};
        esp_err_t ret = esp_lcd_dpi_panel_register_event_callbacks(s_ui_ctx.config.panel_handle, &callbacks, NULL);
        if (ret != ESP_OK)
        {
            ESP_LOGW(TAG, "clear DSI callbacks failed: %s", esp_err_to_name(ret));
        }
    }

    if (s_ui_ctx.display != NULL)
    {
#if CONFIG_HID_TOUCH_ENABLE
        app_touch_unbind_lvgl();
#endif
        esp_err_t ret = lvgl_port_remove_disp(s_ui_ctx.display);
        if (ret != ESP_OK)
        {
            ESP_LOGW(TAG, "lvgl_port_remove_disp failed: %s", esp_err_to_name(ret));
        }
        s_ui_ctx.display = NULL;
    }

    esp_err_t ret = lvgl_port_stop();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE)
    {
        ESP_LOGW(TAG, "lvgl_port_stop failed: %s", esp_err_to_name(ret));
    }

    ret = lvgl_port_deinit();
    if (ret != ESP_OK)
    {
        ESP_LOGW(TAG, "lvgl_port_deinit failed: %s", esp_err_to_name(ret));
    }

    s_ui_ctx.usb_panel = NULL;
    s_ui_ctx.main_panel = NULL;
    s_ui_ctx.usb_label = NULL;
    s_ui_ctx.status_label = NULL;
    s_ui_ctx.countdown_label = NULL;
    s_ui_ctx.countdown_arc = NULL;
    s_ui_ctx.spinner = NULL;
    s_ui_ctx.settings_icon = NULL;
    s_ui_ctx.floating_settings_icon = NULL;
    s_ui_ctx.settings_page = NULL;
    s_ui_ctx.back_icon = NULL;
    s_ui_ctx.hide_switch = NULL;
    s_ui_ctx.hid_switch = NULL;
    s_video_bg = NULL;
    s_countdown_anim_started = false;
    s_ui_ctx.lvgl_ready = false;
    s_ui_ctx.video_output_enabled = true;

    ESP_LOGI(TAG, "LVGL placeholder stopped, switching to USB video");
}

static esp_err_t app_lcd_ui_wait_lvgl_ready(void)
{
    const TickType_t start = xTaskGetTickCount();
    while (!lv_is_initialized())
    {
        if ((xTaskGetTickCount() - start) >= pdMS_TO_TICKS(LCD_UI_LVGL_INIT_WAIT_MS))
        {
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    return ESP_OK;
}

static void app_lcd_ui_task(void *arg)
{
    (void)arg;
    int last_countdown_value = -1;

    while (!s_ui_ctx.video_output_enabled)
    {
        const int64_t now_ms = esp_timer_get_time() / 1000;

        if (!s_ui_ctx.stream_detected)
        {
            if (s_ui_ctx.lvgl_ready && lvgl_port_lock(100))
            {
                app_lcd_ui_update_waiting_status_locked();
                lvgl_port_unlock();
            }
        }
        else
        {
            if (!s_ui_ctx.countdown_started)
            {
                s_ui_ctx.countdown_started = true;
                s_ui_ctx.countdown_start_ms = now_ms;
                last_countdown_value = -1;
                ESP_LOGI(TAG, "USB video stream detected, starting 3-second countdown");
            }

            const int64_t elapsed_ms = now_ms - s_ui_ctx.countdown_start_ms;
            const int seconds_left = elapsed_ms < LCD_UI_COUNTDOWN_MS ? (int)((LCD_UI_COUNTDOWN_MS - elapsed_ms + 999) / 1000) : 0;

            if (seconds_left != last_countdown_value)
            {
                last_countdown_value = seconds_left;
                if (s_ui_ctx.lvgl_ready && lvgl_port_lock(100))
                {
                    app_lcd_ui_update_countdown_locked(seconds_left);
                    lvgl_port_unlock();
                }
            }

            if (elapsed_ms >= LCD_UI_COUNTDOWN_MS)
            {
                bool do_shutdown = false;
                if (s_ui_lock) {
                    xSemaphoreTake(s_ui_lock, portMAX_DELAY);
                }
                if (s_ui_ctx.stream_detected) {
                    s_ui_shutting_down = true;
                    do_shutdown = true;
                } else {
                    /* Stream was cancelled while counting down; stay idle. */
                    s_ui_ctx.countdown_started = false;
                    s_countdown_anim_started = false;
                }
                if (s_ui_lock) {
                    xSemaphoreGive(s_ui_lock);
                }

                if (do_shutdown) {
                    /* The idle-page restore path may re-init LVGL while this
                     * task is still tearing down; don't let it see a stale
                     * handle afterwards. */
                    s_ui_ctx.task_handle = NULL;
                    app_lcd_ui_shutdown_lvgl();
                    if (s_ui_lock) {
                        xSemaphoreTake(s_ui_lock, portMAX_DELAY);
                        s_ui_shutting_down = false;
                        xSemaphoreGive(s_ui_lock);
                    }
                    break;
                }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(LCD_UI_TASK_PERIOD_MS));
    }

    s_ui_ctx.task_handle = NULL;
    vTaskDelete(NULL);
}

esp_err_t app_lcd_ui_init(const app_lcd_ui_config_t *config)
{
    esp_err_t ret = ESP_OK;
    ESP_RETURN_ON_FALSE(config != NULL, ESP_ERR_INVALID_ARG, TAG, "config is null");
    ESP_RETURN_ON_FALSE(config->panel_handle != NULL, ESP_ERR_INVALID_ARG, TAG, "panel handle is null");
    ESP_RETURN_ON_FALSE(config->hres > 0 && config->vres > 0, ESP_ERR_INVALID_ARG, TAG, "invalid panel size");

    if (s_ui_lock == NULL) {
        s_ui_lock = xSemaphoreCreateMutex();
        ESP_RETURN_ON_FALSE(s_ui_lock != NULL, ESP_ERR_NO_MEM, TAG, "ui lock alloc failed");
    }
    s_ui_shutting_down = false;

    memset(&s_ui_ctx, 0, sizeof(s_ui_ctx));
    s_video_bg = NULL;
    s_video_started = false;
    s_ui_ctx.config = *config;
    s_ui_ctx.page_hidden = false;
    s_ui_ctx.hid_touch_enabled = true;
    ret = app_lcd_ui_settings_load();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "load UI settings failed: %s", esp_err_to_name(ret));
        s_ui_ctx.nvs_ready = false;
    }
#if CONFIG_HID_TOUCH_ENABLE
    app_touch_hid_set_enabled(s_ui_ctx.hid_touch_enabled);
#endif

    const lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    ESP_RETURN_ON_ERROR(lvgl_port_init(&lvgl_cfg), TAG, "lvgl port init failed");
    ESP_GOTO_ON_ERROR(app_lcd_ui_wait_lvgl_ready(), fail, TAG, "lvgl init wait failed");

    uint32_t buffer_lines = LCD_UI_DRAW_BUFFER_LINES;
    if (buffer_lines > config->vres)
    {
        buffer_lines = config->vres;
    }

    const lvgl_port_display_cfg_t disp_cfg = {
        .panel_handle = config->panel_handle,
        .control_handle = config->control_handle,
        .buffer_size = config->hres * buffer_lines,
        .double_buffer = false,
        .hres = config->hres,
        .vres = config->vres,
        .monochrome = false,
        .color_format = config->color_format,
        .rotation = {
            .swap_xy = false,
            .mirror_x = false,
            .mirror_y = false,
        },
        .flags = {
            .buff_dma = false,
#if CONFIG_SPIRAM
            .buff_spiram = true,
#else
            .buff_spiram = false,
#endif
            .swap_bytes = false,
        },
    };

    const lvgl_port_display_dsi_cfg_t dsi_cfg = {0};
    s_ui_ctx.display = lvgl_port_add_disp_dsi(&disp_cfg, &dsi_cfg);

    ESP_GOTO_ON_FALSE(s_ui_ctx.display != NULL, ESP_FAIL, fail, TAG, "add lvgl display failed");
    s_ui_ctx.lvgl_ready = true;
#if CONFIG_HID_TOUCH_ENABLE
    ESP_GOTO_ON_ERROR(app_touch_bind_lvgl(s_ui_ctx.display), fail, TAG, "bind touch to LVGL failed");
#endif

    ESP_GOTO_ON_FALSE(lvgl_port_lock(1000), ESP_ERR_TIMEOUT, fail, TAG, "lvgl lock failed");
    app_lcd_ui_build_screen_locked();
    lvgl_port_unlock();

    BaseType_t ok = xTaskCreatePinnedToCore(app_lcd_ui_task,
                                            "lcd_ui",
                                            LCD_UI_TASK_STACK_SIZE,
                                            NULL,
                                            LCD_UI_TASK_PRIORITY,
                                            &s_ui_ctx.task_handle,
                                            tskNO_AFFINITY);
    ESP_GOTO_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, fail, TAG, "create lcd_ui task failed");

    ESP_LOGI(TAG, "LVGL placeholder ready");
    return ESP_OK;

fail:
    s_ui_ctx.video_output_enabled = true;
    if (s_ui_ctx.display != NULL)
    {
#if CONFIG_HID_TOUCH_ENABLE
        app_touch_unbind_lvgl();
#endif
        lvgl_port_remove_disp(s_ui_ctx.display);
        s_ui_ctx.display = NULL;
    }
    lvgl_port_deinit();
    s_ui_ctx.lvgl_ready = false;
    s_video_bg = NULL;
    s_video_started = false;
    return ret;
}

void app_lcd_ui_notify_video_stream(void)
{
    if (!s_ui_ctx.stream_detected)
    {
        s_ui_ctx.stream_detected = true;
    }
}

bool app_lcd_ui_cancel_pending_stream(void)
{
    if (s_ui_lock == NULL) {
        /* Idle UI not initialized yet; nothing to cancel or rebuild. */
        return true;
    }

    if (xSemaphoreTake(s_ui_lock, pdMS_TO_TICKS(50)) != pdTRUE) {
        return false;
    }

    bool canceled = false;
    if (!s_ui_ctx.video_output_enabled && !s_ui_shutting_down) {
        s_ui_ctx.stream_detected = false;
        s_ui_ctx.countdown_started = false;
        s_countdown_anim_started = false;
        canceled = true;
    }
    xSemaphoreGive(s_ui_lock);
    return canceled;
}

bool app_lcd_ui_video_output_enabled(void)
{
    return s_ui_ctx.video_output_enabled;
}

/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esp_check.h"
#include "esp_log.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_lvgl_port.h"
#include "esp_lcd_touch_axs15260.h"
#include "app_lcd.h"
#include "app_usb.h"
#include "usb_descriptors.h"

static const char *TAG = "app_touch";

#if CONFIG_FREERTOS_NUMBER_OF_CORES > 1
#define TOUCH_TASK_CORE 1
#else
#define TOUCH_TASK_CORE tskNO_AFFINITY
#endif

#define EXAMPLE_TOUCH_I2C_SDA        GPIO_NUM_20
#define EXAMPLE_TOUCH_I2C_SCL        GPIO_NUM_21
#define EXAMPLE_TOUCH_RST_GPIO       GPIO_NUM_23
#define EXAMPLE_TOUCH_INT_GPIO       GPIO_NUM_22
#define EXAMPLE_TOUCH_I2C_PORT       I2C_NUM_0
#define TOUCH_FILTER_PRESS_CONFIRM_CNT       2
#define TOUCH_FILTER_RELEASE_CONFIRM_CNT     2
#define TOUCH_FILTER_JITTER_PIXELS           6
#define TOUCH_FILTER_JUMP_PIXELS             80
#define TOUCH_FILTER_RELOCATE_CONFIRM_CNT    2

static axs15260_touch_handle_t tp = NULL;
volatile bool g_hid_touch_filter_enabled = true;
static volatile bool s_hid_touch_report_enabled = true;

typedef enum {
    TOUCH_ROUTE_LVGL,
    TOUCH_ROUTE_HID,
} touch_route_t;

typedef struct {
    uint16_t x;
    uint16_t y;
    uint16_t width;
    uint16_t height;
    uint8_t id;
    bool valid;
} touch_sample_t;

typedef struct {
    bool stable_pressed;
    bool last_report_pressed;
    uint8_t press_confirm_cnt;
    uint8_t release_confirm_cnt;
    uint8_t relocate_confirm_cnt;
    touch_sample_t stable_sample;
    touch_sample_t press_candidate;
    touch_sample_t relocate_candidate;
    touch_sample_t last_report_sample;
} touch_filter_state_t;

static touch_filter_state_t s_touch_filter = {0};
static SemaphoreHandle_t s_touch_lock;
static lv_display_t *s_pending_lvgl_display;
static lv_indev_t *s_lvgl_indev;
static touch_route_t s_touch_route = TOUCH_ROUTE_LVGL;
static bool s_touch_initialized;

static inline uint16_t touch_abs_delta(uint16_t a, uint16_t b)
{
    return (a > b) ? (a - b) : (b - a);
}

static bool touch_samples_near(const touch_sample_t *lhs, const touch_sample_t *rhs, uint16_t threshold)
{
    return touch_abs_delta(lhs->x, rhs->x) <= threshold &&
           touch_abs_delta(lhs->y, rhs->y) <= threshold;
}

static void touch_sample_blend(touch_sample_t *dst, const touch_sample_t *src, uint8_t sample_count)
{
    if (sample_count <= 1) {
        *dst = *src;
        return;
    }

    dst->x = (uint16_t)(((uint32_t)dst->x * (sample_count - 1U) + src->x) / sample_count);
    dst->y = (uint16_t)(((uint32_t)dst->y * (sample_count - 1U) + src->y) / sample_count);
    dst->width = (uint16_t)(((uint32_t)dst->width * (sample_count - 1U) + src->width) / sample_count);
    dst->height = (uint16_t)(((uint32_t)dst->height * (sample_count - 1U) + src->height) / sample_count);
    dst->id = src->id;
    dst->valid = src->valid;
}

static bool touch_extract_sample(const axs15260_touch_data_t *touch_data, touch_sample_t *sample)
{
    if (touch_data->point_num == 0) {
        return false;
    }

    const axs15260_touch_point_t *point = &touch_data->points[0];
    if (point->x >= EXAMPLE_LCD_H_RES || point->y >= EXAMPLE_LCD_V_RES) {
        ESP_LOGW(TAG, "drop out-of-range touch x=%u y=%u", point->x, point->y);
        return false;
    }

    uint16_t size = point->area ? point->area : point->weight;
    sample->x = point->x;
    sample->y = point->y;
    sample->width = size;
    sample->height = size;
    sample->id = point->id;
    sample->valid = true;
    return true;
}

static bool touch_filter_process(const touch_sample_t *raw_sample, touch_sample_t *filtered_sample, bool *pressed)
{
    touch_filter_state_t *state = &s_touch_filter;

    if (!g_hid_touch_filter_enabled) {
        state->press_confirm_cnt = 0;
        state->release_confirm_cnt = 0;
        state->relocate_confirm_cnt = 0;
        state->stable_pressed = (raw_sample != NULL);
        if (raw_sample) {
            state->stable_sample = *raw_sample;
            *filtered_sample = *raw_sample;
            *pressed = true;
        } else {
            *pressed = false;
        }
        return true;
    }

    if (!state->stable_pressed) {
        state->release_confirm_cnt = 0;
        state->relocate_confirm_cnt = 0;

        if (raw_sample == NULL) {
            state->press_confirm_cnt = 0;
            state->press_candidate.valid = false;
            *pressed = false;
            return true;
        }

        if (!state->press_candidate.valid ||
            !touch_samples_near(&state->press_candidate, raw_sample, TOUCH_FILTER_JITTER_PIXELS)) {
            state->press_candidate = *raw_sample;
            state->press_confirm_cnt = 1;
            *pressed = false;
            return false;
        }

        if (state->press_confirm_cnt < UINT8_MAX) {
            state->press_confirm_cnt++;
        }
        touch_sample_blend(&state->press_candidate, raw_sample, state->press_confirm_cnt);
        if (state->press_confirm_cnt < TOUCH_FILTER_PRESS_CONFIRM_CNT) {
            *pressed = false;
            return false;
        }

        state->stable_pressed = true;
        state->press_confirm_cnt = 0;
        state->stable_sample = state->press_candidate;
        state->press_candidate.valid = false;
        *filtered_sample = state->stable_sample;
        *pressed = true;
        return true;
    }

    if (raw_sample == NULL) {
        state->press_confirm_cnt = 0;
        state->relocate_confirm_cnt = 0;
        state->relocate_candidate.valid = false;

        if (state->release_confirm_cnt < UINT8_MAX) {
            state->release_confirm_cnt++;
        }
        if (state->release_confirm_cnt < TOUCH_FILTER_RELEASE_CONFIRM_CNT) {
            *pressed = true;
            *filtered_sample = state->stable_sample;
            return false;
        }

        state->stable_pressed = false;
        state->release_confirm_cnt = 0;
        *pressed = false;
        return true;
    }

    state->release_confirm_cnt = 0;
    state->press_confirm_cnt = 0;

    if (touch_samples_near(&state->stable_sample, raw_sample, TOUCH_FILTER_JITTER_PIXELS)) {
        touch_sample_blend(&state->stable_sample, raw_sample, 4);
        *filtered_sample = state->stable_sample;
        *pressed = true;
        return true;
    }

    if (touch_samples_near(&state->stable_sample, raw_sample, TOUCH_FILTER_JUMP_PIXELS)) {
        state->stable_sample = *raw_sample;
        state->relocate_confirm_cnt = 0;
        state->relocate_candidate.valid = false;
        *filtered_sample = state->stable_sample;
        *pressed = true;
        return true;
    }

    if (!state->relocate_candidate.valid ||
        !touch_samples_near(&state->relocate_candidate, raw_sample, TOUCH_FILTER_JITTER_PIXELS)) {
        state->relocate_candidate = *raw_sample;
        state->relocate_confirm_cnt = 1;
        *pressed = true;
        *filtered_sample = state->stable_sample;
        return false;
    }

    if (state->relocate_confirm_cnt < UINT8_MAX) {
        state->relocate_confirm_cnt++;
    }
    touch_sample_blend(&state->relocate_candidate, raw_sample, state->relocate_confirm_cnt);
    if (state->relocate_confirm_cnt < TOUCH_FILTER_RELOCATE_CONFIRM_CNT) {
        *pressed = true;
        *filtered_sample = state->stable_sample;
        return false;
    }

    state->stable_sample = state->relocate_candidate;
    state->relocate_confirm_cnt = 0;
    state->relocate_candidate.valid = false;
    *filtered_sample = state->stable_sample;
    *pressed = true;
    return true;
}

static void touch_send_hid_report(bool pressed, const touch_sample_t *sample)
{
    hid_report_t report = {0};

    report.report_id = REPORT_ID_TOUCH;
    if (pressed && sample) {
        report.touch_report.cnt = 1;
        report.touch_report.data[0].index = sample->id;
        report.touch_report.data[0].press_down = 1;
        report.touch_report.data[0].x = sample->x;
        report.touch_report.data[0].y = sample->y;
        report.touch_report.data[0].width = sample->width;
        report.touch_report.data[0].height = sample->height;
        ESP_LOGD(TAG, "touch report x=%u y=%u id=%u", sample->x, sample->y, sample->id);
    } else {
        ESP_LOGD(TAG, "touch report release");
    }

#if CFG_TUD_HID
    tinyusb_hid_keyboard_report(report);
#endif
}

static bool touch_should_send_report(bool pressed, const touch_sample_t *sample)
{
    touch_filter_state_t *state = &s_touch_filter;

    if (pressed != state->last_report_pressed) {
        return true;
    }

    if (!pressed || !sample || !state->last_report_sample.valid) {
        return false;
    }

    return sample->x != state->last_report_sample.x ||
           sample->y != state->last_report_sample.y ||
           sample->width != state->last_report_sample.width ||
           sample->height != state->last_report_sample.height ||
           sample->id != state->last_report_sample.id;
}

static void touch_record_report(bool pressed, const touch_sample_t *sample)
{
    s_touch_filter.last_report_pressed = pressed;
    if (pressed && sample) {
        s_touch_filter.last_report_sample = *sample;
    } else {
        s_touch_filter.last_report_sample.valid = false;
    }
}

static esp_err_t touch_read_filtered(touch_sample_t *filtered_sample, bool *filtered_pressed, bool *filter_ready)
{
    axs15260_touch_data_t touch_data = {0};
    touch_sample_t raw_sample = {0};

    ESP_RETURN_ON_FALSE(tp != NULL, ESP_ERR_INVALID_STATE, TAG, "touch not initialized");
    ESP_RETURN_ON_FALSE(filtered_sample != NULL && filtered_pressed != NULL && filter_ready != NULL,
                        ESP_ERR_INVALID_ARG, TAG, "invalid filtered touch args");

    esp_err_t ret = axs15260_touch_read(tp, &touch_data);
    if (ret != ESP_OK) {
        return ret;
    }

    bool has_raw_point = (touch_data.point_num > 0);
    bool raw_valid = touch_extract_sample(&touch_data, &raw_sample);
    if (has_raw_point && !raw_valid) {
        *filter_ready = false;
        *filtered_pressed = false;
        return ESP_OK;
    }

    touch_sample_t *raw_ptr = raw_valid ? &raw_sample : NULL;
    *filter_ready = touch_filter_process(raw_ptr, filtered_sample, filtered_pressed);
    return ESP_OK;
}

static void touch_lvgl_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void) indev;
    touch_sample_t filtered_sample = {0};
    bool filtered_pressed = false;
    bool filter_ready = false;

    esp_err_t ret = touch_read_filtered(&filtered_sample, &filtered_pressed, &filter_ready);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "touch read failed: %s", esp_err_to_name(ret));
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }

    if (!filter_ready && filtered_pressed) {
        ESP_LOGI(TAG, "lvgl touch x=%u y=%u", filtered_sample.x, filtered_sample.y);
        data->point.x = filtered_sample.x;
        data->point.y = filtered_sample.y;
        data->state = LV_INDEV_STATE_PRESSED;
        return;
    }

    if (!filtered_pressed) {
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }

    ESP_LOGI(TAG, "lvgl touch x=%u y=%u", filtered_sample.x, filtered_sample.y);
    data->point.x = filtered_sample.x;
    data->point.y = filtered_sample.y;
    data->state = LV_INDEV_STATE_PRESSED;
}

static esp_err_t app_touch_bind_lvgl_locked(lv_display_t *display)
{
    ESP_RETURN_ON_FALSE(display != NULL, ESP_ERR_INVALID_ARG, TAG, "lvgl display is null");

    s_pending_lvgl_display = display;
    s_touch_route = TOUCH_ROUTE_LVGL;
    memset(&s_touch_filter, 0, sizeof(s_touch_filter));
    if (!s_touch_initialized) {
        return ESP_OK;
    }
    if (s_lvgl_indev != NULL) {
        return ESP_OK;
    }

    ESP_RETURN_ON_FALSE(lvgl_port_lock(1000), ESP_ERR_TIMEOUT, TAG, "lvgl lock failed");
    s_lvgl_indev = lv_indev_create();
    if (s_lvgl_indev) {
        lv_indev_set_type(s_lvgl_indev, LV_INDEV_TYPE_POINTER);
        lv_indev_set_read_cb(s_lvgl_indev, touch_lvgl_read_cb);
        lv_indev_set_disp(s_lvgl_indev, display);
    }
    lvgl_port_unlock();

    ESP_RETURN_ON_FALSE(s_lvgl_indev != NULL, ESP_ERR_NO_MEM, TAG, "create lvgl touch indev failed");
    ESP_LOGI(TAG, "touch routed to LVGL");
    return ESP_OK;
}

static void app_touch_unbind_lvgl_locked(void)
{
    s_pending_lvgl_display = NULL;
    if (s_lvgl_indev != NULL) {
        if (lvgl_port_lock(1000)) {
            lv_indev_delete(s_lvgl_indev);
            lvgl_port_unlock();
        } else {
            ESP_LOGW(TAG, "lvgl lock timeout, touch indev not removed");
        }
        s_lvgl_indev = NULL;
    }
}

static void app_touch_task(void *arg)
{
    (void) arg;
    while (1) {
        touch_sample_t filtered_sample = {0};
        bool filtered_pressed = false;
        bool filter_ready = false;

        if (s_touch_route != TOUCH_ROUTE_HID) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        if (!s_hid_touch_report_enabled) {
            if (s_touch_filter.last_report_pressed) {
                touch_send_hid_report(false, NULL);
                touch_record_report(false, NULL);
            }
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        esp_err_t ret = touch_read_filtered(&filtered_sample, &filtered_pressed, &filter_ready);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "touch read failed: %s", esp_err_to_name(ret));
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        if (filter_ready && touch_should_send_report(filtered_pressed, &filtered_sample)) {
            touch_send_hid_report(filtered_pressed, filtered_pressed ? &filtered_sample : NULL);
            touch_record_report(filtered_pressed, filtered_pressed ? &filtered_sample : NULL);
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

esp_err_t app_touch_init(void)
{
    const axs15260_touch_config_t touch_cfg = {
        .i2c_sda = EXAMPLE_TOUCH_I2C_SDA,
        .i2c_scl = EXAMPLE_TOUCH_I2C_SCL,
        .rst_gpio = EXAMPLE_TOUCH_RST_GPIO,
        .int_gpio = EXAMPLE_TOUCH_INT_GPIO,
        .i2c_port = EXAMPLE_TOUCH_I2C_PORT,
        .x_max = EXAMPLE_LCD_H_RES,
        .y_max = EXAMPLE_LCD_V_RES,
    };

    ESP_RETURN_ON_ERROR(axs15260_touch_new(&touch_cfg, &tp), TAG, "touch init failed");
    s_touch_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_touch_lock != NULL, ESP_ERR_NO_MEM, TAG, "touch lock alloc failed");
    s_touch_route = TOUCH_ROUTE_LVGL;
    s_touch_initialized = true;
    ESP_LOGI(TAG, "touch anti-noise filter default: %s", g_hid_touch_filter_enabled ? "enabled" : "disabled");
    xTaskCreatePinnedToCore(app_touch_task, "app_touch_task", 4096, NULL, CONFIG_TOUCH_TASK_PRIORITY, NULL, TOUCH_TASK_CORE);
    if (s_pending_lvgl_display) {
        xSemaphoreTake(s_touch_lock, portMAX_DELAY);
        esp_err_t ret = app_touch_bind_lvgl_locked(s_pending_lvgl_display);
        xSemaphoreGive(s_touch_lock);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "bind touch to LVGL failed: %s", esp_err_to_name(ret));
        }
    }
    return ESP_OK;
}

esp_err_t app_touch_bind_lvgl(lv_display_t *display)
{
    if (s_touch_lock == NULL) {
        s_pending_lvgl_display = display;
        return ESP_OK;
    }

    xSemaphoreTake(s_touch_lock, portMAX_DELAY);
    esp_err_t ret = app_touch_bind_lvgl_locked(display);
    xSemaphoreGive(s_touch_lock);
    return ret;
}

void app_touch_unbind_lvgl(void)
{
    if (s_touch_lock == NULL) {
        s_pending_lvgl_display = NULL;
        return;
    }

    xSemaphoreTake(s_touch_lock, portMAX_DELAY);
    app_touch_unbind_lvgl_locked();
    xSemaphoreGive(s_touch_lock);
}

void app_touch_route_to_hid(void)
{
    if (s_touch_lock) {
        xSemaphoreTake(s_touch_lock, portMAX_DELAY);
        app_touch_unbind_lvgl_locked();
        s_touch_route = TOUCH_ROUTE_HID;
        xSemaphoreGive(s_touch_lock);
    } else {
        s_touch_route = TOUCH_ROUTE_HID;
    }

    memset(&s_touch_filter, 0, sizeof(s_touch_filter));
    ESP_LOGI(TAG, "touch routed to USB HID");
}

void app_touch_filter_set_enabled(bool enabled)
{
    g_hid_touch_filter_enabled = enabled;
    s_touch_filter.press_confirm_cnt = 0;
    s_touch_filter.release_confirm_cnt = 0;
    s_touch_filter.relocate_confirm_cnt = 0;
    s_touch_filter.press_candidate.valid = false;
    s_touch_filter.relocate_candidate.valid = false;
    ESP_LOGI(TAG, "touch anti-noise filter %s", enabled ? "enabled" : "disabled");
}

bool app_touch_filter_get_enabled(void)
{
    return g_hid_touch_filter_enabled;
}

void app_touch_hid_set_enabled(bool enabled)
{
    s_hid_touch_report_enabled = enabled;
    ESP_LOGI(TAG, "HID touch report %s", enabled ? "enabled" : "disabled");
}

bool app_touch_hid_get_enabled(void)
{
    return s_hid_touch_report_enabled;
}

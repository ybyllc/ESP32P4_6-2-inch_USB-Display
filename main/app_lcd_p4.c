/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <inttypes.h>
#include <string.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/gpio.h"
#include "driver/jpeg_decode.h"
#include "driver/ledc.h"
#include "esp_cache.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_ldo_regulator.h"
#include "esp_lcd_axs15260.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "app_lcd.h"
#include "app_lcd_ui.h"
#if CONFIG_HID_TOUCH_ENABLE
#include "app_touch.h"
#endif
#include "sdkconfig.h"
#include "esp_async_fbcpy.h"

static const char *TAG = "app_lcd";

/* AXS15260D 6.2-inch MIPI board wiring */
#define EXAMPLE_LCD_RST_GPIO                  GPIO_NUM_19
#define LCD_POWER_GPIO                  GPIO_NUM_12
#define EXAMPLE_LCD_BL_GPIO                   GPIO_NUM_25
#define EXAMPLE_LCD_DSI_LDO_CHAN              3
#define EXAMPLE_LCD_DSI_LDO_VOLTAGE_MV        2500

#define EXAMPLE_LCD_BL_LEDC_MODE              LEDC_LOW_SPEED_MODE
#define EXAMPLE_LCD_BL_LEDC_TIMER             LEDC_TIMER_0
#define EXAMPLE_LCD_BL_LEDC_CHANNEL           LEDC_CHANNEL_0
#define EXAMPLE_LCD_BL_LEDC_FREQ_HZ           5000
#define EXAMPLE_LCD_BL_LEDC_DUTY_RES          LEDC_TIMER_13_BIT
#define EXAMPLE_LCD_BL_LEDC_DUTY_MAX          ((1 << 13) - 1)
#define EXAMPLE_JPEG_ALIGN                    16
#define EXAMPLE_JPEG_ALIGN_UP(num, align)     (((num) + ((align) - 1)) & ~((align) - 1))
#define LCD_PENDING_FRAME_QUEUE_LEN           2
#define LCD_READY_FRAME_QUEUE_LEN             2
#define LCD_TASK_STOP_WAIT_MS                 1000

#if CONFIG_FREERTOS_NUMBER_OF_CORES > 1
#define LCD_PIPELINE_TASK_CORE                1
#else
#define LCD_PIPELINE_TASK_CORE                tskNO_AFFINITY
#endif

#define LCD_FLUSH_TASK_PRIORITY               (((CONFIG_VENDOR_TASK_PRIORITY + 2) < (configMAX_PRIORITIES - 1)) ? \
                                              (CONFIG_VENDOR_TASK_PRIORITY + 2) : (configMAX_PRIORITIES - 1))
#define LCD_DECODE_TASK_PRIORITY              LCD_FLUSH_TASK_PRIORITY

typedef struct {
    uint8_t *buffer;
    uint32_t out_size;
} lcd_decode_slot_t;

typedef struct {
    uint32_t submit_ok;
    uint32_t submit_drop;
    uint32_t decode_ok;
    uint32_t decode_fail;
    uint32_t flush_ok;
    uint32_t flush_fail;
} lcd_pipeline_stats_t;

static esp_lcd_panel_handle_t display_handle;
static esp_lcd_panel_handle_t dpi_panel_handle;
static esp_lcd_panel_io_handle_t mipi_io_handle;
static esp_lcd_dsi_bus_handle_t dsi_bus_handle;
static esp_ldo_channel_handle_t dsi_ldo_chan;
static jpeg_decoder_handle_t jpgd_handle;
static esp_async_fbcpy_handle_t s_fbcpy_handle;
static TaskHandle_t s_lcd_copy_task;
static TaskHandle_t s_decode_task_handle;
static TaskHandle_t s_flush_task_handle;
static SemaphoreHandle_t s_pipeline_lock;
static SemaphoreHandle_t s_idle_restore_lock;

static jpeg_decode_cfg_t decode_cfg = {
#if CONFIG_LCD_PIXEL_FORMAT_RGB888
    .output_format = JPEG_DECODE_OUT_FORMAT_RGB888,
#else
    .output_format = JPEG_DECODE_OUT_FORMAT_RGB565,
#endif
    .rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_RGB,
};

static void *lcd_buffer[EXAMPLE_LCD_BUF_NUM];
static lcd_decode_slot_t s_decode_slots[CONFIG_USB_EXTEND_SCREEN_JPEG_DECODE_RING_COUNT];
static QueueHandle_t s_pending_frame_queue;
static QueueHandle_t s_ready_decode_queue;
static QueueHandle_t s_free_decode_slot_queue;
static size_t jpeg_buffer_size;
static uint16_t jpeg_output_h_res;
static uint16_t jpeg_output_v_res;
static uint8_t s_fb_index;
static volatile bool s_decode_busy;
static volatile bool s_flush_busy;
static volatile bool s_stream_pipeline_running;
static bool s_stream_pipeline_active;
static bool s_touch_hid_route_active;
static volatile uint32_t s_flush_submit_count;
static volatile uint32_t s_flush_done_count;
static lcd_pipeline_stats_t s_lcd_stats;
static int64_t s_lcd_last_log_ms;
static app_lcd_ui_config_t s_idle_ui_config;
static bool s_idle_ui_config_valid;

static bool lcd_async_copy_done_cb(esp_async_fbcpy_handle_t mcp, esp_async_fbcpy_event_data_t *event_data, void *cb_args)
{
    BaseType_t high_task_wakeup = pdFALSE;
    TaskHandle_t task = (TaskHandle_t)cb_args;
    (void)mcp;
    (void)event_data;

    if (task) {
        vTaskNotifyGiveFromISR(task, &high_task_wakeup);
    }
    return high_task_wakeup == pdTRUE;
}

static esp_err_t release_decode_slot(uint32_t slot_index)
{
    if (!s_free_decode_slot_queue) {
        return ESP_ERR_INVALID_STATE;
    }
    return xQueueSend(s_free_decode_slot_queue, &slot_index, 0) == pdPASS ? ESP_OK : ESP_FAIL;
}

static void lcd_log_stats_periodic(const char *stage)
{
    const int64_t now_ms = esp_timer_get_time() / 1000;
    if ((now_ms - s_lcd_last_log_ms) < 1000) {
        return;
    }
    s_lcd_last_log_ms = now_ms;

    ESP_LOGI(TAG,
             "pipe[%s] t=%" PRIi64 " ms | submit ok=%" PRIu32 " drop=%" PRIu32
             " | decode ok=%" PRIu32 " fail=%" PRIu32
             " | flush submit=%" PRIu32 " done=%" PRIu32 " ok=%" PRIu32 " fail=%" PRIu32
             " | queue pending=%u ready=%u free=%u | state decode=%d flush=%d fb=%u",
             stage ? stage : "-",
             now_ms,
             s_lcd_stats.submit_ok,
             s_lcd_stats.submit_drop,
             s_lcd_stats.decode_ok,
             s_lcd_stats.decode_fail,
             s_flush_submit_count,
             s_flush_done_count,
             s_lcd_stats.flush_ok,
             s_lcd_stats.flush_fail,
             s_pending_frame_queue ? (unsigned)uxQueueMessagesWaiting(s_pending_frame_queue) : 0,
             s_ready_decode_queue ? (unsigned)uxQueueMessagesWaiting(s_ready_decode_queue) : 0,
             s_free_decode_slot_queue ? (unsigned)uxQueueMessagesWaiting(s_free_decode_slot_queue) : 0,
             s_decode_busy,
             s_flush_busy,
             s_fb_index);
}

static esp_err_t lcd_copy_to_framebuffer(const uint8_t *src_buf, uint8_t *dst_buf)
{
    const size_t bytes_per_pixel = EXAMPLE_LCD_BIT_PER_PIXEL / 8;
    const size_t src_stride = jpeg_output_h_res * bytes_per_pixel;
    const size_t dst_stride = EXAMPLE_LCD_H_RES * bytes_per_pixel;

    if (!s_fbcpy_handle) {
        for (uint16_t row = 0; row < EXAMPLE_LCD_V_RES; row++) {
            memcpy(dst_buf + row * dst_stride,
                   src_buf + row * src_stride,
                   dst_stride);
        }
        return ESP_OK;
    }

    esp_cache_msync((void *)src_buf, jpeg_buffer_size, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
    s_lcd_copy_task = xTaskGetCurrentTaskHandle();

    esp_async_fbcpy_trans_desc_t trans = {
        .src_buffer = (void *)src_buf,
        .dst_buffer = dst_buf,
        .src_buffer_size_x = jpeg_output_h_res,
        .src_buffer_size_y = EXAMPLE_LCD_V_RES,
        .dst_buffer_size_x = EXAMPLE_LCD_H_RES,
        .dst_buffer_size_y = EXAMPLE_LCD_V_RES,
        .src_offset_x = 0,
        .src_offset_y = 0,
        .dst_offset_x = 0,
        .dst_offset_y = 0,
        .copy_size_x = EXAMPLE_LCD_H_RES,
        .copy_size_y = EXAMPLE_LCD_V_RES,
        .pixel_format_unique_id = {
            .color_type_id = decode_cfg.output_format,
        },
    };

    ESP_RETURN_ON_ERROR(esp_async_fbcpy(s_fbcpy_handle, &trans, lcd_async_copy_done_cb, s_lcd_copy_task), TAG, "async framebuffer copy failed");
    ESP_RETURN_ON_FALSE(ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100)) > 0, ESP_ERR_TIMEOUT, TAG, "async framebuffer copy timeout");
    return ESP_OK;
}

static esp_err_t lcd_backlight_init(void)
{
    const ledc_timer_config_t timer_cfg = {
        .speed_mode = EXAMPLE_LCD_BL_LEDC_MODE,
        .timer_num = EXAMPLE_LCD_BL_LEDC_TIMER,
        .duty_resolution = EXAMPLE_LCD_BL_LEDC_DUTY_RES,
        .freq_hz = EXAMPLE_LCD_BL_LEDC_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer_cfg), TAG, "backlight timer init failed");

    const ledc_channel_config_t channel_cfg = {
        .gpio_num = EXAMPLE_LCD_BL_GPIO,
        .speed_mode = EXAMPLE_LCD_BL_LEDC_MODE,
        .channel = EXAMPLE_LCD_BL_LEDC_CHANNEL,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = EXAMPLE_LCD_BL_LEDC_TIMER,
        .duty = 0,
        .hpoint = 0,
    };
    ESP_RETURN_ON_ERROR(ledc_channel_config(&channel_cfg), TAG, "backlight channel init failed");
    return ESP_OK;
}

static esp_err_t lcd_backlight_set(uint8_t brightness_percent)
{
    const uint32_t duty = (EXAMPLE_LCD_BL_LEDC_DUTY_MAX * brightness_percent) / 100;
    ESP_RETURN_ON_ERROR(ledc_set_duty(EXAMPLE_LCD_BL_LEDC_MODE, EXAMPLE_LCD_BL_LEDC_CHANNEL, duty), TAG, "set backlight duty failed");
    ESP_RETURN_ON_ERROR(ledc_update_duty(EXAMPLE_LCD_BL_LEDC_MODE, EXAMPLE_LCD_BL_LEDC_CHANNEL), TAG, "update backlight duty failed");
    return ESP_OK;
}

static void lcd_decode_task(void *arg)
{
    (void)arg;

    while (s_stream_pipeline_running) {
        frame_t *frame = NULL;
        uint32_t slot_index = 0;

        if (xQueueReceive(s_pending_frame_queue, &frame, pdMS_TO_TICKS(100)) != pdPASS) {
            continue;
        }
        if (!s_stream_pipeline_running) {
            if (frame) {
                frame_return_empty(frame);
            }
            break;
        }
        if (frame == NULL) {
            continue;
        }
        while (s_stream_pipeline_running &&
               xQueueReceive(s_free_decode_slot_queue, &slot_index, pdMS_TO_TICKS(20)) != pdPASS) {
        }
        if (!s_stream_pipeline_running) {
            frame_return_empty(frame);
            break;
        }

        s_decode_busy = true;
        esp_err_t ret = jpeg_decoder_process(jpgd_handle, &decode_cfg, frame->data, frame->info.total,
                                             s_decode_slots[slot_index].buffer, jpeg_buffer_size,
                                             &s_decode_slots[slot_index].out_size);
        s_decode_busy = false;
        frame_return_empty(frame);

        if (ret != ESP_OK) {
            s_lcd_stats.decode_fail++;
            ESP_LOGW(TAG, "decode fail: err=%s(%d) jpeg_len=%" PRIu32 " slot=%" PRIu32,
                     esp_err_to_name(ret), ret, frame->info.total, slot_index);
            lcd_log_stats_periodic("decode_fail");
            release_decode_slot(slot_index);
            continue;
        }

        s_lcd_stats.decode_ok++;
        lcd_log_stats_periodic("decode_ok");

        uint32_t stale_slot = 0;
        while (xQueueReceive(s_ready_decode_queue, &stale_slot, 0) == pdPASS) {
            release_decode_slot(stale_slot);
        }

        if (xQueueSend(s_ready_decode_queue, &slot_index, 0) != pdPASS) {
            release_decode_slot(slot_index);
        }
    }

    s_decode_busy = false;
    s_decode_task_handle = NULL;
    vTaskDelete(NULL);
}

static void lcd_flush_task(void *arg)
{
    (void)arg;

    while (s_stream_pipeline_running) {
        uint32_t slot_index = 0;
        if (xQueueReceive(s_ready_decode_queue, &slot_index, pdMS_TO_TICKS(100)) != pdPASS) {
            continue;
        }
        if (!s_stream_pipeline_running || slot_index == UINT32_MAX) {
            break;
        }

        s_flush_busy = true;
        s_flush_submit_count++;
        uint8_t fb_index = s_fb_index;
        esp_err_t ret = lcd_copy_to_framebuffer(s_decode_slots[slot_index].buffer, (uint8_t *)lcd_buffer[fb_index]);
        if (ret == ESP_OK) {
            ret = esp_lcd_panel_draw_bitmap(dpi_panel_handle, 0, 0, EXAMPLE_LCD_H_RES, EXAMPLE_LCD_V_RES, lcd_buffer[fb_index]);
        }
        s_flush_busy = false;

        if (ret == ESP_OK) {
            s_fb_index = (fb_index + 1) == EXAMPLE_LCD_BUF_NUM ? 0 : (fb_index + 1);
            s_flush_done_count++;
            s_lcd_stats.flush_ok++;
#if CONFIG_HID_TOUCH_ENABLE
            if (!s_touch_hid_route_active) {
                app_touch_route_to_hid();
                s_touch_hid_route_active = true;
            }
#endif
            lcd_log_stats_periodic("flush_ok");
        } else {
            s_lcd_stats.flush_fail++;
            ESP_LOGW(TAG, "flush fail: err=%s(%d) slot=%" PRIu32 " fb=%u",
                     esp_err_to_name(ret), ret, slot_index, fb_index);
            lcd_log_stats_periodic("flush_fail");
        }
        release_decode_slot(slot_index);
    }

    s_flush_busy = false;
    s_flush_task_handle = NULL;
    vTaskDelete(NULL);
}

static esp_err_t lcd_wait_task_exit(TaskHandle_t *task_handle)
{
    TickType_t start = xTaskGetTickCount();
    while (*task_handle != NULL) {
        if ((xTaskGetTickCount() - start) >= pdMS_TO_TICKS(LCD_TASK_STOP_WAIT_MS)) {
            vTaskDelete(*task_handle);
            *task_handle = NULL;
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    return ESP_OK;
}

static void lcd_drain_pending_frames(void)
{
    frame_t *frame = NULL;
    if (s_pending_frame_queue) {
        while (xQueueReceive(s_pending_frame_queue, &frame, 0) == pdPASS) {
            if (frame) {
                frame_return_empty(frame);
            }
        }
    }
}

static void lcd_free_stream_buffers(void)
{
    for (uint32_t i = 0; i < CONFIG_USB_EXTEND_SCREEN_JPEG_DECODE_RING_COUNT; i++) {
        if (s_decode_slots[i].buffer) {
            heap_caps_free(s_decode_slots[i].buffer);
            s_decode_slots[i].buffer = NULL;
        }
        s_decode_slots[i].out_size = 0;
    }
    jpeg_buffer_size = 0;
    jpeg_output_h_res = 0;
    jpeg_output_v_res = 0;
}

static esp_err_t lcd_stream_pipeline_start_locked(void)
{
    esp_err_t ret = ESP_OK;

    if (s_stream_pipeline_active) {
        return ESP_OK;
    }

    const jpeg_decode_engine_cfg_t decode_eng_cfg = {
        .intr_priority = 1,
        .timeout_ms = 50,
    };
    jpeg_decode_memory_alloc_cfg_t jpeg_mem_cfg = {
        .buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER,
    };
    esp_async_fbcpy_config_t fbcpy_cfg = {};

    s_pending_frame_queue = xQueueCreate(LCD_PENDING_FRAME_QUEUE_LEN, sizeof(frame_t *));
    s_ready_decode_queue = xQueueCreate(LCD_READY_FRAME_QUEUE_LEN, sizeof(uint32_t));
    s_free_decode_slot_queue = xQueueCreate(CONFIG_USB_EXTEND_SCREEN_JPEG_DECODE_RING_COUNT, sizeof(uint32_t));
    ESP_GOTO_ON_FALSE(s_pending_frame_queue && s_ready_decode_queue && s_free_decode_slot_queue,
                      ESP_ERR_NO_MEM, fail, TAG, "queue alloc failed");

    ESP_GOTO_ON_ERROR(jpeg_new_decoder_engine(&decode_eng_cfg, &jpgd_handle), fail, TAG, "jpeg decoder init failed");
    if (esp_async_fbcpy_install(&fbcpy_cfg, &s_fbcpy_handle) != ESP_OK) {
        s_fbcpy_handle = NULL;
    }

    jpeg_output_h_res = EXAMPLE_JPEG_ALIGN_UP(EXAMPLE_LCD_H_RES, EXAMPLE_JPEG_ALIGN);
    jpeg_output_v_res = EXAMPLE_JPEG_ALIGN_UP(EXAMPLE_LCD_V_RES, EXAMPLE_JPEG_ALIGN);
    jpeg_buffer_size = jpeg_output_h_res * jpeg_output_v_res * EXAMPLE_LCD_BIT_PER_PIXEL / 8;
    for (uint32_t i = 0; i < CONFIG_USB_EXTEND_SCREEN_JPEG_DECODE_RING_COUNT; i++) {
        s_decode_slots[i].buffer = jpeg_alloc_decoder_mem(jpeg_buffer_size, &jpeg_mem_cfg, &jpeg_buffer_size);
        ESP_GOTO_ON_FALSE(s_decode_slots[i].buffer != NULL, ESP_ERR_NO_MEM, fail, TAG, "alloc jpeg buffer failed");
        ESP_GOTO_ON_FALSE(xQueueSend(s_free_decode_slot_queue, &i, 0) == pdPASS, ESP_FAIL, fail, TAG, "decode slot queue init failed");
    }

    s_fb_index = 0;
    s_decode_busy = false;
    s_flush_busy = false;
    s_flush_submit_count = 0;
    s_flush_done_count = 0;
    memset(&s_lcd_stats, 0, sizeof(s_lcd_stats));
    s_lcd_last_log_ms = 0;
    s_stream_pipeline_running = true;

    BaseType_t ok = xTaskCreatePinnedToCore(lcd_decode_task, "lcd_decode", 4096, NULL,
                                            LCD_DECODE_TASK_PRIORITY, &s_decode_task_handle, LCD_PIPELINE_TASK_CORE);
    ESP_GOTO_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, fail, TAG, "create lcd_decode task failed");
    ok = xTaskCreatePinnedToCore(lcd_flush_task, "lcd_flush", 4096, NULL,
                                 LCD_FLUSH_TASK_PRIORITY, &s_flush_task_handle, LCD_PIPELINE_TASK_CORE);
    ESP_GOTO_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, fail, TAG, "create lcd_flush task failed");

    s_stream_pipeline_active = true;
    ESP_LOGI(TAG, "stream pipeline ready: jpeg %ux%u (%u bytes), decode ring=%u",
             jpeg_output_h_res, jpeg_output_v_res, (unsigned)jpeg_buffer_size,
             (unsigned)CONFIG_USB_EXTEND_SCREEN_JPEG_DECODE_RING_COUNT);
    return ESP_OK;

fail:
    s_stream_pipeline_running = false;
    lcd_drain_pending_frames();
    if (s_decode_task_handle) {
        lcd_wait_task_exit(&s_decode_task_handle);
    }
    if (s_flush_task_handle) {
        lcd_wait_task_exit(&s_flush_task_handle);
    }
    if (s_fbcpy_handle) {
        esp_async_fbcpy_uninstall(s_fbcpy_handle);
        s_fbcpy_handle = NULL;
    }
    if (jpgd_handle) {
        jpeg_del_decoder_engine(jpgd_handle);
        jpgd_handle = NULL;
    }
    lcd_free_stream_buffers();
    if (s_pending_frame_queue) {
        vQueueDelete(s_pending_frame_queue);
        s_pending_frame_queue = NULL;
    }
    if (s_ready_decode_queue) {
        vQueueDelete(s_ready_decode_queue);
        s_ready_decode_queue = NULL;
    }
    if (s_free_decode_slot_queue) {
        vQueueDelete(s_free_decode_slot_queue);
        s_free_decode_slot_queue = NULL;
    }
    return ret;
}

static void lcd_stream_pipeline_stop_locked(void)
{
    if (!s_stream_pipeline_active && !s_pending_frame_queue && !jpgd_handle) {
        return;
    }

    s_stream_pipeline_running = false;
    if (s_pending_frame_queue) {
        frame_t *stop_frame = NULL;
        xQueueSend(s_pending_frame_queue, &stop_frame, 0);
    }
    if (s_ready_decode_queue) {
        uint32_t stop_slot = UINT32_MAX;
        xQueueSend(s_ready_decode_queue, &stop_slot, 0);
    }

    esp_err_t decode_ret = lcd_wait_task_exit(&s_decode_task_handle);
    esp_err_t flush_ret = lcd_wait_task_exit(&s_flush_task_handle);
    if (decode_ret != ESP_OK || flush_ret != ESP_OK) {
        ESP_LOGW(TAG, "stream task stop timeout decode=%s flush=%s",
                 esp_err_to_name(decode_ret), esp_err_to_name(flush_ret));
    }

    lcd_drain_pending_frames();

    if (s_fbcpy_handle) {
        esp_async_fbcpy_uninstall(s_fbcpy_handle);
        s_fbcpy_handle = NULL;
    }
    if (jpgd_handle) {
        jpeg_del_decoder_engine(jpgd_handle);
        jpgd_handle = NULL;
    }

    lcd_free_stream_buffers();

    if (s_pending_frame_queue) {
        vQueueDelete(s_pending_frame_queue);
        s_pending_frame_queue = NULL;
    }
    if (s_ready_decode_queue) {
        vQueueDelete(s_ready_decode_queue);
        s_ready_decode_queue = NULL;
    }
    if (s_free_decode_slot_queue) {
        vQueueDelete(s_free_decode_slot_queue);
        s_free_decode_slot_queue = NULL;
    }

    s_decode_busy = false;
    s_flush_busy = false;
    s_stream_pipeline_active = false;
    s_touch_hid_route_active = false;
    ESP_LOGI(TAG, "stream pipeline stopped");
}

esp_err_t app_lcd_submit_frame(frame_t *frame)
{
#if CONFIG_USB_EXTEND_SCREEN_USB_STABILITY_TEST_MODE
    return frame ? frame_return_empty(frame) : ESP_ERR_INVALID_ARG;
#else
    if (!frame) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!app_lcd_ui_video_output_enabled()) {
        return frame_return_empty(frame);
    }
    if (app_lcd_prepare_stream_mode() != ESP_OK) {
        return frame_return_empty(frame);
    }
    if (!s_stream_pipeline_active || !jpgd_handle || !s_pending_frame_queue) {
        return frame_return_empty(frame);
    }

    frame_t *stale_frame = NULL;
    while (xQueueReceive(s_pending_frame_queue, &stale_frame, 0) == pdPASS) {
        if (stale_frame) {
            frame_return_empty(stale_frame);
            s_lcd_stats.submit_drop++;
        }
    }

    if (xQueueSend(s_pending_frame_queue, &frame, 0) == pdPASS) {
        s_lcd_stats.submit_ok++;
        lcd_log_stats_periodic("submit_ok");
        return ESP_OK;
    }

    s_lcd_stats.submit_drop++;
    lcd_log_stats_periodic("submit_drop");
    return ESP_FAIL;
#endif
}

esp_err_t app_lcd_prepare_stream_mode(void)
{
#if CONFIG_USB_EXTEND_SCREEN_USB_STABILITY_TEST_MODE
    return ESP_OK;
#else
    if (!app_lcd_ui_video_output_enabled()) {
        return ESP_ERR_INVALID_STATE;
    }
    ESP_RETURN_ON_FALSE(s_pipeline_lock != NULL, ESP_ERR_INVALID_STATE, TAG, "pipeline lock not ready");

    esp_err_t ret = ESP_OK;
    xSemaphoreTake(s_pipeline_lock, portMAX_DELAY);
    if (!s_stream_pipeline_active) {
        ret = lcd_stream_pipeline_start_locked();
    }
    xSemaphoreGive(s_pipeline_lock);
    return ret;
#endif
}

bool app_lcd_should_drop_frame(void)
{
#if CONFIG_USB_EXTEND_SCREEN_USB_STABILITY_TEST_MODE
    return false;
#else
    if (!app_lcd_ui_video_output_enabled()) {
        return true;
    }
    if (!s_stream_pipeline_active) {
        return true;
    }
    if (!jpgd_handle || !s_pending_frame_queue || !s_ready_decode_queue || !s_free_decode_slot_queue) {
        return true;
    }

    return s_decode_busy ||
           s_flush_busy ||
           (s_flush_submit_count == 0 && s_flush_done_count == 0 && uxQueueMessagesWaiting(s_ready_decode_queue) > 0) ||
           uxQueueSpacesAvailable(s_pending_frame_queue) == 0 ||
           uxQueueSpacesAvailable(s_ready_decode_queue) == 0 ||
           uxQueueMessagesWaiting(s_free_decode_slot_queue) == 0;
#endif
}

uint32_t app_lcd_get_flush_count(void)
{
#if CONFIG_USB_EXTEND_SCREEN_USB_STABILITY_TEST_MODE
    return 0;
#else
    return s_flush_done_count;
#endif
}

void app_lcd_notify_video_stream_start(void)
{
#if !CONFIG_USB_EXTEND_SCREEN_USB_STABILITY_TEST_MODE
    app_lcd_ui_notify_video_stream();
#endif
}

void app_lcd_notify_video_stream_lost(void)
{
#if !CONFIG_USB_EXTEND_SCREEN_USB_STABILITY_TEST_MODE
    if (s_pipeline_lock == NULL) {
        ESP_LOGW(TAG, "pipeline lock not ready");
        return;
    }

    xSemaphoreTake(s_pipeline_lock, portMAX_DELAY);
    lcd_stream_pipeline_stop_locked();
    xSemaphoreGive(s_pipeline_lock);

    if (!s_idle_ui_config_valid || s_idle_restore_lock == NULL) {
        return;
    }

    /* The transfer task exit, the USB umount callback and the USB health
     * monitor can all race here; only one may restore the idle UI. */
    if (xSemaphoreTake(s_idle_restore_lock, 0) != pdTRUE) {
        ESP_LOGD(TAG, "idle UI restore already in progress");
        return;
    }

    if (!app_lcd_ui_cancel_pending_stream()) {
        /* The UI already switched to video mode (or is being torn down):
         * wait for the teardown to finish, then rebuild the idle page. */
        while (!app_lcd_ui_video_output_enabled()) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        esp_err_t ret = app_lcd_ui_init(&s_idle_ui_config);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "restart idle UI failed: %s", esp_err_to_name(ret));
        }
    }

    xSemaphoreGive(s_idle_restore_lock);
#endif
}

esp_err_t app_lcd_init(void)
{
#if CONFIG_USB_EXTEND_SCREEN_USB_STABILITY_TEST_MODE
    ESP_LOGW(TAG, "USB stability test mode: skip ESP32-P4 LCD/DSI init");
    return ESP_OK;
#else
    esp_log_level_set("jpeg.decoder", ESP_LOG_NONE);
    gpio_set_direction(LCD_POWER_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(LCD_POWER_GPIO, 0);

    s_pipeline_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_pipeline_lock != NULL, ESP_ERR_NO_MEM, TAG, "pipeline lock alloc failed");
    s_idle_restore_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_idle_restore_lock != NULL, ESP_ERR_NO_MEM, TAG, "idle restore lock alloc failed");

    ESP_RETURN_ON_ERROR(lcd_backlight_init(), TAG, "backlight init failed");
    ESP_RETURN_ON_ERROR(lcd_backlight_set(0), TAG, "backlight off failed");

    const esp_ldo_channel_config_t ldo_cfg = {
        .chan_id = EXAMPLE_LCD_DSI_LDO_CHAN,
        .voltage_mv = EXAMPLE_LCD_DSI_LDO_VOLTAGE_MV,
    };
    ESP_RETURN_ON_ERROR(esp_ldo_acquire_channel(&ldo_cfg, &dsi_ldo_chan), TAG, "mipi ldo init failed");
    // vTaskDelay(pdMS_TO_TICKS(100));

    const esp_lcd_dsi_bus_config_t bus_cfg = AXS15260_PANEL_BUS_DSI_2CH_CONFIG();
    ESP_RETURN_ON_ERROR(esp_lcd_new_dsi_bus(&bus_cfg, &dsi_bus_handle), TAG, "create dsi bus failed");

    const esp_lcd_dbi_io_config_t dbi_cfg = AXS15260_PANEL_IO_DBI_CONFIG();
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_dbi(dsi_bus_handle, &dbi_cfg, &mipi_io_handle), TAG, "create dsi dbi io failed");

    esp_lcd_dpi_panel_config_t dpi_cfg =
#if CONFIG_LCD_PIXEL_FORMAT_RGB888
        AXS15260_452_1280_PANEL_60HZ_DPI_CONFIG(LCD_COLOR_PIXEL_FORMAT_RGB888);
    dpi_cfg.in_color_format = LCD_COLOR_FMT_RGB888;
    dpi_cfg.out_color_format = LCD_COLOR_FMT_RGB888;
#else
        AXS15260_452_1280_PANEL_60HZ_DPI_CONFIG(LCD_COLOR_PIXEL_FORMAT_RGB565);
    dpi_cfg.in_color_format = LCD_COLOR_FMT_RGB565;
    dpi_cfg.out_color_format = LCD_COLOR_FMT_RGB565;
#endif
    dpi_cfg.num_fbs = EXAMPLE_LCD_BUF_NUM;

    axs15260_vendor_config_t vendor_cfg = {
        .mipi_config = {
            .dsi_bus = dsi_bus_handle,
            .dpi_config = &dpi_cfg,
            .lane_num = AXS15260_MIPI_LANES,
        },
        .flags.use_mipi_interface = 1,
    };

    const esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = EXAMPLE_LCD_RST_GPIO,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
        .bits_per_pixel = EXAMPLE_LCD_BIT_PER_PIXEL,
        .vendor_config = &vendor_cfg,
    };

    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_axs15260(mipi_io_handle, &panel_cfg, &display_handle), TAG, "create axs15260 panel failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(display_handle), TAG, "init axs15260 panel failed");
    dpi_panel_handle = esp_lcd_axs15260_get_dpi_panel(display_handle);
    ESP_RETURN_ON_FALSE(dpi_panel_handle != NULL, ESP_FAIL, TAG, "get dpi panel handle failed");

    ESP_RETURN_ON_ERROR(lcd_backlight_set(100), TAG, "backlight on failed");

#if EXAMPLE_LCD_BUF_NUM == 1
    ESP_RETURN_ON_ERROR(esp_lcd_dpi_panel_get_frame_buffer(dpi_panel_handle, 1, &lcd_buffer[0]), TAG, "get lcd framebuffer failed");
#elif EXAMPLE_LCD_BUF_NUM == 2
    ESP_RETURN_ON_ERROR(esp_lcd_dpi_panel_get_frame_buffer(dpi_panel_handle, 2, &lcd_buffer[0], &lcd_buffer[1]), TAG, "get lcd framebuffer failed");
#else
    ESP_RETURN_ON_ERROR(esp_lcd_dpi_panel_get_frame_buffer(dpi_panel_handle, 3, &lcd_buffer[0], &lcd_buffer[1], &lcd_buffer[2]), TAG, "get lcd framebuffer failed");
#endif

    ESP_LOGI(TAG, "AXS15260 panel ready, resolution: %dx%d", EXAMPLE_LCD_H_RES, EXAMPLE_LCD_V_RES);
    s_idle_ui_config = (app_lcd_ui_config_t) {
        .panel_type = APP_LCD_UI_PANEL_DSI,
        .panel_handle = dpi_panel_handle,
        .control_handle = display_handle,
        .hres = EXAMPLE_LCD_H_RES,
        .vres = EXAMPLE_LCD_V_RES,
#if CONFIG_LCD_PIXEL_FORMAT_RGB888
        .color_format = LV_COLOR_FORMAT_RGB888,
#else
        .color_format = LV_COLOR_FORMAT_RGB565,
#endif
    };
    s_idle_ui_config_valid = true;
    ESP_RETURN_ON_ERROR(app_lcd_ui_init(&s_idle_ui_config), TAG, "lvgl placeholder init failed");
    return ESP_OK;
#endif
}

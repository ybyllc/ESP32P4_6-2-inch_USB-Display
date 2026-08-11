/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <inttypes.h>
#include <string.h>
#include "esp_check.h"
#include "esp_log.h"
#include "app_usb.h"
#include "app_usb_health.h"
#include "app_lcd.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "usb_frame.h"

static const char *TAG = "app_vendor";
static frame_t *current_frame = NULL;
static bool s_skip_frame = false;
static frame_info_t s_skip_frame_info = {0};
static app_vendor_stats_t s_vendor_stats = {0};
static int64_t s_last_stream_rx_ms = 0;
static int64_t s_vendor_last_rx_log_ms = 0;
static TaskHandle_t s_transfer_task_handle = NULL;
static SemaphoreHandle_t s_vendor_lock = NULL;
static volatile bool s_stream_rx_active = false;
static volatile bool s_transfer_task_running = false;

#define TRANSFER_TASK_POLL_MS 100

#if CONFIG_FREERTOS_NUMBER_OF_CORES > 1
#define DISPLAY_TASK_CORE 1
#else
#define DISPLAY_TASK_CORE tskNO_AFFINITY
#endif

//--------------------------------------------------------------------+
// Vendor callbacks
//--------------------------------------------------------------------+

// -- Display Packets
#define UDISP_TYPE_RGB565  0
#define UDISP_TYPE_RGB888  1
#define UDISP_TYPE_YUV420  2
#define UDISP_TYPE_JPG     3
#define UDISP_TYPE_END     0xff

typedef struct {
    uint16_t crc16;
    uint8_t  type;
    uint8_t cmd;
    uint16_t x;
    uint16_t y;
    uint16_t width;
    uint16_t height;
    uint32_t frame_id: 10;
    uint32_t payload_total: 22; //padding 32bit align
} __attribute__((packed)) udisp_frame_header_t;

static bool buffer_skip(frame_info_t *frame_info, uint32_t len);

static void app_vendor_reset_rx_state_locked(void)
{
    if (current_frame) {
        frame_return_empty(current_frame);
        current_frame = NULL;
    }
    s_skip_frame = false;
    memset(&s_skip_frame_info, 0, sizeof(s_skip_frame_info));
}

static void app_vendor_note_stream_rx_locked(void)
{
    s_last_stream_rx_ms = esp_timer_get_time() / 1000;
}

static void app_vendor_log_rx_periodic(const char *stage)
{
    const int64_t now_ms = esp_timer_get_time() / 1000;
    if ((now_ms - s_vendor_last_rx_log_ms) < 1000) {
        return;
    }
    s_vendor_last_rx_log_ms = now_ms;

    ESP_LOGI(TAG,
             "rx[%s] t=%" PRIi64 " ms | rx_cb=%" PRIu32 " rx_bytes=%" PRIu32
             " started=%" PRIu32 " done=%" PRIu32 " drop_bp=%" PRIu32 " no_empty=%" PRIu32
             " stale=%" PRIu32 " short=%" PRIu32 " bad_hdr=%" PRIu32
             " geom=%" PRIu32 " payload=%" PRIu32 " queue_err=%" PRIu32
             " | queue empty=%u filled=%u | frame=%s %" PRIu32 "/%" PRIu32,
             stage ? stage : "-",
             now_ms,
             s_vendor_stats.rx_callbacks,
             s_vendor_stats.rx_bytes,
             s_vendor_stats.frames_started,
             s_vendor_stats.frames_completed,
             s_vendor_stats.frames_dropped_backpressure,
             s_vendor_stats.frames_dropped_no_empty,
             s_vendor_stats.stale_frames_skipped,
             s_vendor_stats.short_packets,
             s_vendor_stats.invalid_headers,
             s_vendor_stats.geometry_mismatch,
             s_vendor_stats.payload_errors,
             s_vendor_stats.queue_errors,
             (unsigned)frame_empty_count(),
             (unsigned)frame_filled_count(),
             current_frame ? "yes" : "no",
             current_frame ? current_frame->info.received : 0,
             current_frame ? current_frame->info.total : 0);
}

static void app_vendor_dump_stats_internal(const char *reason)
{
    const app_vendor_stats_t stats = s_vendor_stats;
    const frame_info_t current_info = current_frame ? current_frame->info : (frame_info_t){0};

    ESP_LOGI(TAG,
             "snapshot[%s] t=%" PRIi64 " ms | rx_cb=%" PRIu32 " rx_bytes=%" PRIu32
             " started=%" PRIu32 " done=%" PRIu32 " drop_bp=%" PRIu32 " no_empty=%" PRIu32
             " stale=%" PRIu32 " short=%" PRIu32 " bad_hdr=%" PRIu32
             " geom=%" PRIu32 " payload=%" PRIu32 " queue_err=%" PRIu32
             " | queue empty=%u filled=%u"
             " | state current=%s %" PRIu32 "/%" PRIu32 " skip=%s %" PRIu32 "/%" PRIu32,
             reason ? reason : "-",
             esp_timer_get_time() / 1000,
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
             stats.queue_errors,
             (unsigned)frame_empty_count(),
             (unsigned)frame_filled_count(),
             current_frame ? "yes" : "no",
             current_info.received,
             current_info.total,
             s_skip_frame ? "yes" : "no",
             s_skip_frame_info.received,
             s_skip_frame_info.total);
}

void app_vendor_get_stats(app_vendor_stats_t *stats)
{
    if (!stats) {
        return;
    }
    *stats = s_vendor_stats;
}

void app_vendor_dump_stats(const char *reason)
{
    app_vendor_dump_stats_internal(reason);
}

void transfer_task(void *pvParameter)
{
    (void)pvParameter;
    frame_t *usr_frame = NULL;
    size_t stale_dropped = 0;

    while (s_transfer_task_running) {
        usr_frame = frame_get_latest_filled_timeout(&stale_dropped, pdMS_TO_TICKS(TRANSFER_TASK_POLL_MS));
        if (!usr_frame) {
            if (s_stream_rx_active && !app_usb_host_alive()) {
                ESP_LOGW(TAG,
                         "USB host lost (no SOF), return to idle UI (mounted=%d suspended=%d ready=%d)",
                         tud_mounted(),
                         tud_suspended(),
                         tud_ready());
                break;
            }
            continue;
        }
        if (stale_dropped > 0) {
            s_vendor_stats.stale_frames_skipped += stale_dropped;
            app_vendor_log_rx_periodic("stale_drop");
        }
#if !CONFIG_USB_EXTEND_SCREEN_USB_STABILITY_TEST_MODE
        if (app_lcd_submit_frame(usr_frame) != ESP_OK) {
            s_vendor_stats.frames_dropped_backpressure++;
            frame_return_empty(usr_frame);
            app_vendor_log_rx_periodic("submit_drop");
        }
#else
        frame_return_empty(usr_frame);
#endif
    }

    if (s_vendor_lock) {
        xSemaphoreTake(s_vendor_lock, portMAX_DELAY);
        s_stream_rx_active = false;
        s_transfer_task_running = false;
        app_vendor_reset_rx_state_locked();
        xSemaphoreGive(s_vendor_lock);
    }

    app_lcd_notify_video_stream_lost();
    frame_deallocate();
    s_transfer_task_handle = NULL;
    vTaskDelete(NULL);
}

void app_vendor_notify_usb_unmounted(void)
{
    if (!s_vendor_lock) {
        return;
    }

    if (xSemaphoreTake(s_vendor_lock, pdMS_TO_TICKS(20)) != pdTRUE) {
        ESP_LOGW(TAG, "USB unmount: vendor lock busy, defer stream stop to transfer task");
        return;
    }

    const bool stream_active = s_stream_rx_active || s_transfer_task_running;
    bool notify_lcd = false;

    if (stream_active) {
        ESP_LOGW(TAG, "USB host lost, request video stream stop");
        s_transfer_task_running = false;
        if (s_transfer_task_handle == NULL) {
            /* No transfer task to perform the cleanup: stop inline. */
            s_stream_rx_active = false;
            app_vendor_reset_rx_state_locked();
            frame_deallocate();
            notify_lcd = true;
        }
        /* If a transfer task exists it will clean up and notify on exit. */
    } else {
        /* No RX stream, but the idle UI may be waiting for a stream that never
         * started, or the screen may already be in video mode without a
         * transfer task: let the LCD layer restore the idle page. */
        notify_lcd = true;
    }

    xSemaphoreGive(s_vendor_lock);

    if (notify_lcd) {
        app_lcd_notify_video_stream_lost();
    }
}

static void start_skip_frame(uint32_t payload_total, uint32_t first_payload_len)
{
    memset(&s_skip_frame_info, 0, sizeof(s_skip_frame_info));
    s_skip_frame_info.total = payload_total;
    s_skip_frame = true;
    if (first_payload_len > 0 && buffer_skip(&s_skip_frame_info, first_payload_len)) {
        s_skip_frame = false;
    }
}

static esp_err_t app_vendor_start_stream_locked(void)
{
    if (s_stream_rx_active) {
        return ESP_OK;
    }
    if (s_transfer_task_handle != NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = app_lcd_prepare_stream_mode();
    if (ret != ESP_OK) {
        return ret;
    }

    ret = frame_allocate(CONFIG_USB_EXTEND_SCREEN_RX_FRAME_BUFFER_COUNT, CONFIG_USB_EXTEND_SCREEN_FRAME_LIMIT_B);
    if (ret != ESP_OK) {
        app_lcd_notify_video_stream_lost();
        return ret;
    }

    s_stream_rx_active = true;
    s_transfer_task_running = true;
    app_vendor_note_stream_rx_locked();

    BaseType_t ok = xTaskCreatePinnedToCore(transfer_task, "transfer_task", 4096, NULL,
                                            CONFIG_VENDOR_TASK_PRIORITY, &s_transfer_task_handle, DISPLAY_TASK_CORE);
    if (ok != pdPASS) {
        s_stream_rx_active = false;
        s_transfer_task_running = false;
        frame_deallocate();
        app_lcd_notify_video_stream_lost();
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "USB stream RX buffers ready: count=%u size=%u",
             (unsigned)CONFIG_USB_EXTEND_SCREEN_RX_FRAME_BUFFER_COUNT,
             (unsigned)CONFIG_USB_EXTEND_SCREEN_FRAME_LIMIT_B);
    return ESP_OK;
}

static bool should_drop_new_frame(void)
{
    if (frame_filled_count() > 0) {
        return true;
    }

    if (!app_lcd_should_drop_frame()) {
        return false;
    }

    return frame_empty_count() <= 4;
}

static bool buffer_skip(frame_info_t *frame_info, uint32_t len)
{
    if (frame_info->received + len >= frame_info->total) {
        return true;
    }
    frame_info->received += len;
    return false;
}

static bool buffer_fill(frame_t *frame, const uint8_t *buf, uint32_t len)
{
    if (0 == len) {
        return false;
    }

    if (frame_add_data(frame, buf, len) != ESP_OK) {
        s_vendor_stats.payload_errors++;
        frame_return_empty(frame);
        return true;
    }
    frame->info.received += len;

    if (frame->info.received >= frame->info.total) {
        if (frame_send_filled(frame) != ESP_OK) {
            s_vendor_stats.queue_errors++;
            frame_return_empty(frame);
        } else {
            s_vendor_stats.frames_completed++;
        }
        return true;
    }
    return false;
}

static void app_vendor_rx_process(const uint8_t *rx_buf, int read_res)
{
    if (read_res <= 0) {
        return;
    }

    s_vendor_stats.rx_callbacks++;
    s_vendor_stats.rx_bytes += read_res;

    if (!current_frame && !s_skip_frame) {
        if (read_res < sizeof(udisp_frame_header_t)) {
            s_vendor_stats.short_packets++;
            return;
        }

        const udisp_frame_header_t *pblt = (const udisp_frame_header_t *)rx_buf;
        switch (pblt->type) {
        case UDISP_TYPE_RGB565:
        case UDISP_TYPE_RGB888:
        case UDISP_TYPE_YUV420:
        case UDISP_TYPE_JPG: {
            if (pblt->x != 0 || pblt->y != 0 || pblt->width != EXAMPLE_LCD_H_RES || pblt->height != EXAMPLE_LCD_V_RES) {
                s_vendor_stats.geometry_mismatch++;
                ESP_LOGW(TAG, "frame geometry mismatch: x=%u y=%u w=%u h=%u expect=%ux%u",
                         pblt->x, pblt->y, pblt->width, pblt->height, EXAMPLE_LCD_H_RES, EXAMPLE_LCD_V_RES);
                break;
            }

            app_lcd_notify_video_stream_start();
            if (app_vendor_start_stream_locked() != ESP_OK) {
                start_skip_frame(pblt->payload_total, read_res - sizeof(udisp_frame_header_t));
                app_vendor_log_rx_periodic("wait_stream_mode");
                break;
            }
            app_vendor_note_stream_rx_locked();

            if (should_drop_new_frame()) {
                s_vendor_stats.frames_dropped_backpressure++;
                start_skip_frame(pblt->payload_total, read_res - sizeof(udisp_frame_header_t));
                app_vendor_log_rx_periodic("backpressure");
                break;
            }

            current_frame = frame_get_empty();
            if (current_frame) {
                s_vendor_stats.frames_started++;
                current_frame->info.width = pblt->width;
                current_frame->info.height = pblt->height;
                current_frame->info.total = pblt->payload_total;
                current_frame->info.received = 0;
                ESP_LOGD(TAG, "rx packet: len=%d type=%u cmd=%u x=%u y=%u w=%u h=%u total=%"PRIu32,
                         read_res, pblt->type, pblt->cmd, pblt->x, pblt->y, pblt->width, pblt->height, current_frame->info.total);
                if (buffer_fill(current_frame, &rx_buf[sizeof(udisp_frame_header_t)], read_res - sizeof(udisp_frame_header_t))) {
                    current_frame = NULL;
                    app_vendor_log_rx_periodic("frame_done");
                }
            } else {
                start_skip_frame(pblt->payload_total, read_res - sizeof(udisp_frame_header_t));
                s_vendor_stats.frames_dropped_no_empty++;
                app_vendor_log_rx_periodic("no_empty");
            }
            break;
        }
        case UDISP_TYPE_END:
            break;
        default:
            s_vendor_stats.invalid_headers++;
            ESP_LOGE(TAG, "unexpected vendor header #%" PRIu32 ": len=%d type=0x%02x cmd=0x%02x bytes=%02x %02x %02x %02x %02x %02x %02x %02x",
                     s_vendor_stats.invalid_headers, read_res, rx_buf[2], rx_buf[3],
                     rx_buf[0], rx_buf[1], rx_buf[2], rx_buf[3], rx_buf[4], rx_buf[5], rx_buf[6], rx_buf[7]);
            break;
        }
    } else if (s_skip_frame) {
        app_vendor_note_stream_rx_locked();
        if (buffer_skip(&s_skip_frame_info, read_res)) {
            current_frame = NULL;
            s_skip_frame = false;
            app_vendor_log_rx_periodic("skip_done");
        }
    } else {
        app_vendor_note_stream_rx_locked();
        if (buffer_fill(current_frame, rx_buf, read_res)) {
            current_frame = NULL;
            app_vendor_log_rx_periodic("frame_done");
        }
    }
}

void tud_vendor_rx_cb(uint8_t itf, uint8_t const *buffer, uint16_t bufsize)
{
    if (s_vendor_lock && xSemaphoreTake(s_vendor_lock, pdMS_TO_TICKS(20)) != pdTRUE) {
#if CFG_TUD_VENDOR_RX_BUFSIZE > 0
        tud_vendor_n_read_flush(itf);
#endif
        return;
    }

#if CFG_TUD_VENDOR_RX_BUFSIZE > 0
    /* FIFO mode: this TinyUSB vendor class delivers data through an internal
     * FIFO and invokes the callback with a NULL buffer. Drain whatever has
     * arrived, then flush so unprocessed bytes are dropped and the next
     * transfer is re-armed. */
    uint8_t chunk[512];
    while (tud_vendor_n_available(itf) > 0) {
        const uint32_t len = tud_vendor_n_read(itf, chunk, sizeof(chunk));
        if (len == 0) {
            break;
        }
        app_vendor_rx_process(chunk, (int)len);
    }
    tud_vendor_n_read_flush(itf);
#else
    /* Non-FIFO mode: buffer is valid for this transfer. */
    app_vendor_rx_process(buffer, bufsize);
#endif

    if (s_vendor_lock) {
        xSemaphoreGive(s_vendor_lock);
    }
}

esp_err_t app_vendor_init(void)
{
    s_vendor_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_vendor_lock != NULL, ESP_ERR_NO_MEM, TAG, "vendor lock alloc failed");
    return ESP_OK;
}

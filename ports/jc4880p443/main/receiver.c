#include "receiver.h"
#include "udisp_protocol.h"
#include "driver/jpeg_decode.h"
#include "esp_cache.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_mipi_dsi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "tusb.h"
#include <string.h>
#if !defined(ESP_LCD_DPI_HAS_FRAME_BUF_COMPLETE_CB)
#error "本工程需要仓库固定SDK的真实DMA切帧完成补丁，禁止用普通VSYNC代替"
#endif
static const char *TAG = "usb_display";
static QueueHandle_t empty_frames, ready_frames;
static SemaphoreHandle_t switched;
static portMUX_TYPE switch_lock = portMUX_INITIALIZER_UNLOCKED;
static bool switch_pending;
static udisp_parser_t parser;
static udisp_frame_t frames[3];
static esp_lcd_panel_handle_t lcd;
static void *display_buffers[2];
static jpeg_decoder_handle_t decoder;
static uint32_t displayed, decode_errors;

static udisp_frame_t *acquire(void *context) {
    (void)context; udisp_frame_t *frame = NULL;
    xQueueReceive(empty_frames, &frame, 0);
    return frame;
}
static void release(void *context, udisp_frame_t *frame) {
    (void)context;
    configASSERT(xQueueSend(empty_frames, &frame, 0) == pdTRUE);
}
static void complete(void *context, udisp_frame_t *frame) {
    /* 待解码队列只有一帧，新帧替换旧帧；正在解码的借用帧不动。 */
    udisp_frame_t *stale = NULL;
    if (xQueueReceive(ready_frames, &stale, 0) == pdTRUE) release(context, stale);
    if (xQueueSend(ready_frames, &frame, 0) != pdTRUE) release(context, frame);
}
static bool frame_complete(esp_lcd_panel_handle_t panel, esp_lcd_dpi_panel_event_data_t *event, void *context) {
    (void)panel; (void)event; (void)context;
    BaseType_t wake = pdFALSE;
    portENTER_CRITICAL_ISR(&switch_lock);
    bool pending = switch_pending;
    switch_pending = false;
    portEXIT_CRITICAL_ISR(&switch_lock);
    if (pending) xSemaphoreGiveFromISR(switched, &wake);
    return wake == pdTRUE;
}
static void decode_task(void *context) {
    (void)context;
    unsigned back = 1;
    bool display_fault = false;
    /* IDF此字段控制字节序：RGB=大端、BGR=小端，并非只交换红蓝通道。
     * DPI原生RGB565帧缓冲要求小端；大端会让白色FFFF正常，其它颜色错乱。 */
    const jpeg_decode_cfg_t config = {.output_format=JPEG_DECODE_OUT_FORMAT_RGB565,
                                      .rgb_order=JPEG_DEC_RGB_ELEMENT_ORDER_BGR,
                                      .conv_std=JPEG_YUV_RGB_CONV_STD_BT601};
    for (;;) {
        udisp_frame_t *frame;
        xQueueReceive(ready_frames, &frame, portMAX_DELAY);
        jpeg_decode_picture_info_t info;
        uint32_t output_size = 0;
        if (display_fault) { release(NULL, frame); continue; }
        esp_err_t decode_result = jpeg_decoder_get_info(frame->data, frame->length, &info);
        if (decode_result == ESP_OK && (info.width != UDISP_WIDTH || info.height != UDISP_HEIGHT)) {
            if (++decode_errors <= 5)
                ESP_LOGW(TAG, "JPEG dimensions %lux%lu, expected 480x800",
                         (unsigned long)info.width, (unsigned long)info.height);
            release(NULL, frame); continue;
        }
        if (decode_result == ESP_OK)
            decode_result = jpeg_decoder_process(decoder, &config, frame->data, frame->length,
                                                  display_buffers[back], UDISP_WIDTH*UDISP_HEIGHT*2, &output_size);
        if (decode_result != ESP_OK || output_size != UDISP_WIDTH*UDISP_HEIGHT*2) {
            if (++decode_errors <= 5)
                ESP_LOGW(TAG, "JPEG decode failed result=%s output=%lu", esp_err_to_name(decode_result),
                         (unsigned long)output_size);
            release(NULL, frame); continue;
        }
        release(NULL, frame);
        /* JPEG DMA写入PSRAM后先丢弃CPU旧缓存；draw_bitmap的C2M不能把上轮像素写回去。 */
        if (esp_cache_msync(display_buffers[back], UDISP_WIDTH*UDISP_HEIGHT*2,
                            ESP_CACHE_MSYNC_FLAG_DIR_M2C) != ESP_OK) { ++decode_errors; continue; }
        /* 先登记再提交，完成ISR可早于draw_bitmap返回；绝不提前复用正在扫描的前台。 */
        xSemaphoreTake(switched, 0);
        portENTER_CRITICAL(&switch_lock); switch_pending = true; portEXIT_CRITICAL(&switch_lock);
        esp_err_t ret = esp_lcd_panel_draw_bitmap(lcd, 0, 0, UDISP_WIDTH, UDISP_HEIGHT, display_buffers[back]);
        if (ret != ESP_OK) {
            portENTER_CRITICAL(&switch_lock); switch_pending = false; portEXIT_CRITICAL(&switch_lock);
            ++decode_errors; continue;
        }
        if (xSemaphoreTake(switched, pdMS_TO_TICKS(250)) != pdTRUE) {
            /* 不确定DMA是否切换时停止写LCD缓冲，USB仍可枚举；不能靠假完成继续覆盖。 */
            display_fault = true;
            ESP_LOGE(TAG, "frame switch timed out; display frozen safely, reset required");
            continue;
        }
        back ^= 1;
        if (++displayed == 1 || displayed % 150 == 0)
            ESP_LOGI(TAG, "displayed=%lu decode_errors=%lu",
                     (unsigned long)displayed, (unsigned long)decode_errors);
    }
}
void tud_vendor_rx_cb(uint8_t interface, uint8_t const *buffer, uint16_t length) {
    (void)interface;
    static TickType_t last_report;
    static bool reported;
    TickType_t now = xTaskGetTickCount();
    if (!parser.remaining && length >= UDISP_HEADER_SIZE &&
        (!reported || now-last_report >= pdMS_TO_TICKS(2000))) {
        reported = true; last_report = now;
        ESP_LOGI(TAG, "RX header type=%u size=%ux%u packet=%u accepted=%lu rejected=%lu",
                 buffer[2], buffer[8] | (buffer[9]<<8), buffer[10] | (buffer[11]<<8), length,
                 (unsigned long)parser.accepted, (unsigned long)parser.invalid);
    }
    /* TinyUSB任务内串行解析，绝不在ISR里解码或调用LCD。 */
    udisp_parser_packet(&parser, buffer, length);
}
void tud_umount_cb(void) { udisp_parser_reset(&parser); }
void tud_mount_cb(void) { udisp_parser_reset(&parser); }
esp_err_t usb_display_receiver_init(esp_lcd_panel_handle_t panel, void *first, void *second) {
    lcd = panel; display_buffers[0] = first; display_buffers[1] = second;
    empty_frames = xQueueCreate(3, sizeof(udisp_frame_t *));
    ready_frames = xQueueCreate(1, sizeof(udisp_frame_t *));
    switched = xSemaphoreCreateBinary();
    if (!empty_frames || !ready_frames || !switched) return ESP_ERR_NO_MEM;
    for (unsigned i = 0; i < 3; ++i) {
        size_t capacity = 0;
        jpeg_decode_memory_alloc_cfg_t allocation = {.buffer_direction=JPEG_DEC_ALLOC_INPUT_BUFFER};
        frames[i].data = jpeg_alloc_decoder_mem(UDISP_FRAME_LIMIT, &allocation, &capacity);
        if (!frames[i].data) return ESP_ERR_NO_MEM;
        frames[i].capacity = capacity;
        udisp_frame_t *frame = &frames[i]; xQueueSend(empty_frames, &frame, 0);
    }
    parser.pool = (udisp_pool_t){.acquire=acquire, .complete=complete, .release=release};
    jpeg_decode_engine_cfg_t engine = {.timeout_ms=100};
    ESP_RETURN_ON_ERROR(jpeg_new_decoder_engine(&engine, &decoder), TAG, "JPEG engine");
    memset(first, 0, UDISP_WIDTH*UDISP_HEIGHT*2); memset(second, 0, UDISP_WIDTH*UDISP_HEIGHT*2);
    ESP_RETURN_ON_ERROR(esp_cache_msync(first, UDISP_WIDTH*UDISP_HEIGHT*2, ESP_CACHE_MSYNC_FLAG_DIR_C2M), TAG, "cache");
    ESP_RETURN_ON_ERROR(esp_cache_msync(second, UDISP_WIDTH*UDISP_HEIGHT*2, ESP_CACHE_MSYNC_FLAG_DIR_C2M), TAG, "cache");
    esp_lcd_dpi_panel_event_callbacks_t events = {.on_frame_buf_complete=frame_complete};
    ESP_RETURN_ON_ERROR(esp_lcd_dpi_panel_register_event_callbacks(lcd, &events, NULL), TAG, "frame callback");
    if (xTaskCreatePinnedToCore(decode_task, "usb_jpeg_lcd", 6144, NULL, 5, NULL, 1) != pdPASS)
        return ESP_ERR_NO_MEM;
    return ESP_OK;
}

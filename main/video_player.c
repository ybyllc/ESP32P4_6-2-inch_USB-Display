#include "video_player.h"

#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "app_assets.h"
#include "driver/jpeg_decode.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "misc/cache/instance/lv_image_cache.h"

#define VIDEO_MAGIC "LVGLMJPG"
#define VIDEO_RING_BUFFER_COUNT 6
#define VIDEO_DECODE_TASK_STACK 8192
#define VIDEO_DECODE_TASK_PRIO 5
#define VIDEO_HEADER_SIZE 64
#define VIDEO_STOP_WAIT_MS 500

typedef struct __attribute__((packed)) {
    char magic[8];
    uint16_t width;
    uint16_t height;
    uint16_t fps_num;
    uint16_t fps_den;
    uint32_t frame_count;
    uint32_t index_offset;
    uint32_t frames_offset;
    uint32_t max_frame_size;
    uint32_t flags;
    uint8_t reserved[28];
} video_file_header_t;

typedef struct __attribute__((packed)) {
    uint32_t offset;
    uint32_t size;
} video_frame_index_t;

typedef enum {
    VIDEO_SLOT_FREE = 0,
    VIDEO_SLOT_DECODING,
    VIDEO_SLOT_READY,
    VIDEO_SLOT_DISPLAYING,
} video_slot_state_t;

typedef struct {
    uint8_t *buf;
    lv_image_dsc_t dsc;
    video_slot_state_t state;
    uint32_t seq;
    size_t buf_size;
} video_frame_slot_t;

typedef struct {
    FILE *file;
    video_file_header_t header;
    video_frame_index_t *index;
    uint8_t *jpg_buf;
    size_t jpg_buf_size;
    size_t frame_buf_size;
    video_frame_slot_t slots[VIDEO_RING_BUFFER_COUNT];
    jpeg_decoder_handle_t decoder;
    portMUX_TYPE mux;
    lv_obj_t *image;
    lv_timer_t *display_timer;
    TaskHandle_t decode_task_handle;
    TaskHandle_t stats_task_handle;
    int displayed_slot;
    int next_frame_id;
    bool running;
    uint32_t next_seq;
    uint32_t decoded_frames;
    uint32_t displayed_frames;
    uint32_t decode_waits;
    uint32_t display_misses;
    uint32_t display_timer_ticks;
} video_player_t;

static const char *TAG = "video_player";
static video_player_t s_video;

static uint32_t align16(uint32_t value)
{
    return (value + 15U) & ~15U;
}

static esp_err_t video_read_exact(video_player_t *player, uint32_t offset, void *buffer, size_t size)
{
    ESP_RETURN_ON_FALSE(player->file != NULL, ESP_ERR_INVALID_STATE, TAG, "video file is not open");
    ESP_RETURN_ON_FALSE(buffer != NULL, ESP_ERR_INVALID_ARG, TAG, "read buffer is null");

    ESP_RETURN_ON_FALSE(app_assets_lock(pdMS_TO_TICKS(10000)), ESP_ERR_TIMEOUT, TAG, "assets lock timeout");
    esp_err_t ret = ESP_OK;
    if (fseek(player->file, (long)offset, SEEK_SET) != 0) {
        ret = ESP_FAIL;
    } else if (fread(buffer, 1, size, player->file) != size) {
        ret = ESP_FAIL;
    }
    app_assets_unlock();

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "read %u + %u from %s failed, errno=%d", offset, (unsigned)size,
                 APP_ASSETS_VIDEO_PATH, errno);
    }
    return ret;
}

static esp_err_t video_read_header(video_player_t *player)
{
    ESP_RETURN_ON_ERROR(app_assets_init(), TAG, "assets init failed");

    player->file = fopen(APP_ASSETS_VIDEO_PATH, "rb");
    ESP_RETURN_ON_FALSE(player->file != NULL, ESP_ERR_NOT_FOUND, TAG, "video file not found: %s", APP_ASSETS_VIDEO_PATH);

    ESP_RETURN_ON_ERROR(video_read_exact(player, 0, &player->header, sizeof(player->header)),
                        TAG, "read video header failed");
    ESP_RETURN_ON_FALSE(memcmp(player->header.magic, VIDEO_MAGIC, sizeof(player->header.magic)) == 0,
                        ESP_ERR_INVALID_VERSION, TAG, "invalid video stream magic");
    ESP_RETURN_ON_FALSE(player->header.width > 0 && player->header.height > 0 &&
                        player->header.frame_count > 0 && player->header.max_frame_size > 0,
                        ESP_ERR_INVALID_SIZE, TAG, "invalid video stream header");
    ESP_RETURN_ON_FALSE(player->header.index_offset >= VIDEO_HEADER_SIZE &&
                        player->header.frames_offset > player->header.index_offset,
                        ESP_ERR_INVALID_SIZE, TAG, "invalid video stream offsets");

    ESP_LOGI(TAG, "video: %ux%u, %u/%u fps, %" PRIu32 " frames, max jpg %" PRIu32 " bytes",
             player->header.width, player->header.height,
             player->header.fps_num, player->header.fps_den,
             player->header.frame_count, player->header.max_frame_size);
    return ESP_OK;
}

static esp_err_t video_alloc_buffers(video_player_t *player)
{
    size_t index_size = player->header.frame_count * sizeof(video_frame_index_t);
    player->index = heap_caps_malloc(index_size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    ESP_RETURN_ON_FALSE(player->index, ESP_ERR_NO_MEM, TAG, "alloc frame index failed");
    ESP_RETURN_ON_ERROR(video_read_exact(player, player->header.index_offset, player->index, index_size),
                        TAG, "read frame index failed");

    jpeg_decode_memory_alloc_cfg_t in_mem_cfg = {
        .buffer_direction = JPEG_DEC_ALLOC_INPUT_BUFFER,
    };
    size_t jpg_allocated = 0;
    player->jpg_buf = jpeg_alloc_decoder_mem(player->header.max_frame_size, &in_mem_cfg, &jpg_allocated);
    ESP_RETURN_ON_FALSE(player->jpg_buf, ESP_ERR_NO_MEM, TAG, "alloc jpeg input buffer failed");
    player->jpg_buf_size = jpg_allocated;

    uint32_t out_w = align16(player->header.width);
    uint32_t out_h = align16(player->header.height);
    player->frame_buf_size = out_w * out_h * 3U;
    jpeg_decode_memory_alloc_cfg_t out_mem_cfg = {
        .buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER,
    };
    for (int i = 0; i < VIDEO_RING_BUFFER_COUNT; i++) {
        video_frame_slot_t *slot = &player->slots[i];
        size_t out_allocated = 0;
        slot->buf = jpeg_alloc_decoder_mem(player->frame_buf_size, &out_mem_cfg, &out_allocated);
        ESP_RETURN_ON_FALSE(slot->buf, ESP_ERR_NO_MEM, TAG, "alloc rgb frame buffer failed");
        slot->buf_size = out_allocated;

        memset(&slot->dsc, 0, sizeof(slot->dsc));
        slot->dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
        slot->dsc.header.cf = LV_COLOR_FORMAT_RGB888;
        slot->dsc.header.w = player->header.width;
        slot->dsc.header.h = player->header.height;
        slot->dsc.header.stride = out_w * 3U;
        slot->dsc.data_size = player->frame_buf_size;
        slot->dsc.data = slot->buf;
        slot->state = VIDEO_SLOT_FREE;
        slot->seq = 0;
    }

    return ESP_OK;
}

static esp_err_t video_decode_frame(video_player_t *player, uint32_t frame_id, uint8_t *out_buf)
{
    video_frame_index_t entry = player->index[frame_id];
    ESP_RETURN_ON_FALSE(entry.size > 0 && entry.size <= player->jpg_buf_size,
                        ESP_ERR_INVALID_SIZE, TAG, "invalid frame size");
    ESP_RETURN_ON_ERROR(video_read_exact(player, entry.offset, player->jpg_buf, entry.size),
                        TAG, "read jpeg frame failed");

    jpeg_decode_picture_info_t picture_info = {0};
    ESP_RETURN_ON_ERROR(jpeg_decoder_get_info(player->jpg_buf, entry.size, &picture_info),
                        TAG, "parse jpeg frame failed");
    ESP_RETURN_ON_FALSE(picture_info.width == player->header.width &&
                        picture_info.height == player->header.height,
                        ESP_ERR_INVALID_SIZE, TAG, "jpeg frame resolution changed");

    jpeg_decode_cfg_t decode_cfg = {
        .output_format = JPEG_DECODE_OUT_FORMAT_RGB888,
        .rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_RGB,
        .conv_std = JPEG_YUV_RGB_CONV_STD_BT709,
    };
    uint32_t out_size = 0;
    ESP_RETURN_ON_ERROR(jpeg_decoder_process(player->decoder, &decode_cfg, player->jpg_buf, entry.size,
                                             out_buf, player->frame_buf_size, &out_size),
                        TAG, "hardware jpeg decode failed");
    ESP_RETURN_ON_FALSE(out_size <= player->frame_buf_size, ESP_ERR_INVALID_SIZE,
                        TAG, "jpeg output larger than frame buffer");
    return ESP_OK;
}

static int video_find_decode_slot(video_player_t *player)
{
    portENTER_CRITICAL(&player->mux);
    for (int i = 0; i < VIDEO_RING_BUFFER_COUNT; i++) {
        if (player->slots[i].state == VIDEO_SLOT_FREE) {
            player->slots[i].state = VIDEO_SLOT_DECODING;
            portEXIT_CRITICAL(&player->mux);
            return i;
        }
    }

    player->decode_waits++;
    portEXIT_CRITICAL(&player->mux);
    return -1;
}

static void video_mark_ready(video_player_t *player, int slot_id)
{
    portENTER_CRITICAL(&player->mux);
    player->slots[slot_id].seq = ++player->next_seq;
    player->slots[slot_id].state = VIDEO_SLOT_READY;
    player->decoded_frames++;
    portEXIT_CRITICAL(&player->mux);
}

static int video_take_oldest_ready(video_player_t *player)
{
    int best = -1;
    uint32_t oldest_seq = UINT32_MAX;

    portENTER_CRITICAL(&player->mux);
    for (int i = 0; i < VIDEO_RING_BUFFER_COUNT; i++) {
        if (player->slots[i].state == VIDEO_SLOT_READY && player->slots[i].seq < oldest_seq) {
            oldest_seq = player->slots[i].seq;
            best = i;
        }
    }

    if (best >= 0) {
        if (player->displayed_slot >= 0 && player->displayed_slot != best) {
            player->slots[player->displayed_slot].state = VIDEO_SLOT_FREE;
        }
        player->slots[best].state = VIDEO_SLOT_DISPLAYING;
        player->displayed_slot = best;
        player->displayed_frames++;
    }
    portEXIT_CRITICAL(&player->mux);
    return best;
}

static void video_count_slots(video_player_t *player, int *free_count, int *decoding_count,
                              int *ready_count, int *displaying_count)
{
    *free_count = 0;
    *decoding_count = 0;
    *ready_count = 0;
    *displaying_count = 0;

    portENTER_CRITICAL(&player->mux);
    for (int i = 0; i < VIDEO_RING_BUFFER_COUNT; i++) {
        switch (player->slots[i].state) {
        case VIDEO_SLOT_FREE:
            (*free_count)++;
            break;
        case VIDEO_SLOT_DECODING:
            (*decoding_count)++;
            break;
        case VIDEO_SLOT_READY:
            (*ready_count)++;
            break;
        case VIDEO_SLOT_DISPLAYING:
            (*displaying_count)++;
            break;
        }
    }
    portEXIT_CRITICAL(&player->mux);
}

static void video_stats_task(void *arg)
{
    video_player_t *player = (video_player_t *)arg;
    uint32_t last_decoded = 0;
    uint32_t last_displayed = 0;
    uint32_t last_decode_waits = 0;
    uint32_t last_misses = 0;
    uint32_t last_timer_ticks = 0;

    while (player->running) {
        vTaskDelay(pdMS_TO_TICKS(2000));

        if (!player->running) {
            break;
        }

        int free_count = 0;
        int decoding_count = 0;
        int ready_count = 0;
        int displaying_count = 0;
        uint32_t decoded = 0;
        uint32_t displayed = 0;
        uint32_t decode_waits = 0;
        uint32_t misses = 0;
        uint32_t timer_ticks = 0;

        video_count_slots(player, &free_count, &decoding_count, &ready_count, &displaying_count);

        portENTER_CRITICAL(&player->mux);
        decoded = player->decoded_frames;
        displayed = player->displayed_frames;
        decode_waits = player->decode_waits;
        misses = player->display_misses;
        timer_ticks = player->display_timer_ticks;
        portEXIT_CRITICAL(&player->mux);

        ESP_LOGI(TAG,
                 "ring free=%d decoding=%d ready=%d displaying=%d | tick=%" PRIu32 "/2s dec=%" PRIu32 "/2s disp=%" PRIu32 "/2s dec_wait=%" PRIu32 " miss=%" PRIu32 " heap=%lu psram=%lu",
                 free_count, decoding_count, ready_count, displaying_count,
                 timer_ticks - last_timer_ticks,
                 decoded - last_decoded,
                 displayed - last_displayed,
                 decode_waits - last_decode_waits,
                 misses - last_misses,
                 (unsigned long)esp_get_free_heap_size(),
                 (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

        last_decoded = decoded;
        last_displayed = displayed;
        last_decode_waits = decode_waits;
        last_misses = misses;
        last_timer_ticks = timer_ticks;
    }

    portENTER_CRITICAL(&player->mux);
    player->stats_task_handle = NULL;
    portEXIT_CRITICAL(&player->mux);
    vTaskDelete(NULL);
}

static void video_decode_task(void *arg)
{
    video_player_t *player = (video_player_t *)arg;

    while (player->running) {
        int slot_id = video_find_decode_slot(player);
        if (slot_id < 0) {
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }

        int frame_id = player->next_frame_id;
        player->next_frame_id++;
        if (player->next_frame_id >= (int)player->header.frame_count) {
            player->next_frame_id = 0;
        }

        esp_err_t ret = video_decode_frame(player, frame_id, player->slots[slot_id].buf);
        if (ret == ESP_OK) {
            video_mark_ready(player, slot_id);
        } else {
            ESP_LOGW(TAG, "decode frame %d failed: %s", frame_id, esp_err_to_name(ret));
            portENTER_CRITICAL(&player->mux);
            player->slots[slot_id].state = VIDEO_SLOT_FREE;
            portEXIT_CRITICAL(&player->mux);
        }
    }

    portENTER_CRITICAL(&player->mux);
    player->decode_task_handle = NULL;
    portEXIT_CRITICAL(&player->mux);
    vTaskDelete(NULL);
}

static void video_display_timer_cb(lv_timer_t *timer)
{
    video_player_t *player = (video_player_t *)lv_timer_get_user_data(timer);
    if (!player->running) {
        return;
    }

    portENTER_CRITICAL(&player->mux);
    player->display_timer_ticks++;
    portEXIT_CRITICAL(&player->mux);

    int slot_id = video_take_oldest_ready(player);
    if (slot_id < 0) {
        portENTER_CRITICAL(&player->mux);
        player->display_misses++;
        portEXIT_CRITICAL(&player->mux);
        return;
    }

    lv_image_cache_drop(&player->slots[slot_id].dsc);
    lv_image_set_src(player->image, &player->slots[slot_id].dsc);
    lv_obj_invalidate(player->image);
}

static esp_err_t video_bind_image(video_player_t *player, lv_obj_t *image, int first_slot)
{
    ESP_RETURN_ON_FALSE(image, ESP_ERR_INVALID_ARG, TAG, "image object is null");
    player->image = image;
    lv_image_set_src(player->image, &player->slots[first_slot].dsc);
    return ESP_OK;
}

static void video_free_buffers(video_player_t *player)
{
    for (int i = 0; i < VIDEO_RING_BUFFER_COUNT; i++) {
        if (player->slots[i].buf) {
            heap_caps_free(player->slots[i].buf);
            player->slots[i].buf = NULL;
        }
        memset(&player->slots[i].dsc, 0, sizeof(player->slots[i].dsc));
        player->slots[i].buf_size = 0;
        player->slots[i].state = VIDEO_SLOT_FREE;
        player->slots[i].seq = 0;
    }

    if (player->jpg_buf) {
        heap_caps_free(player->jpg_buf);
        player->jpg_buf = NULL;
    }
    player->jpg_buf_size = 0;

    if (player->index) {
        heap_caps_free(player->index);
        player->index = NULL;
    }
}

static esp_err_t video_wait_task_exit(TaskHandle_t *task_handle)
{
    TickType_t start = xTaskGetTickCount();
    while (*task_handle != NULL) {
        if ((xTaskGetTickCount() - start) >= pdMS_TO_TICKS(VIDEO_STOP_WAIT_MS)) {
            vTaskDelete(*task_handle);
            *task_handle = NULL;
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    return ESP_OK;
}

esp_err_t video_player_start(lv_obj_t *image)
{
    video_player_t *player = &s_video;
    ESP_RETURN_ON_FALSE(!player->running, ESP_ERR_INVALID_STATE, TAG, "video player already running");
    memset(player, 0, sizeof(*player));
    player->mux = (portMUX_TYPE)portMUX_INITIALIZER_UNLOCKED;
    player->displayed_slot = -1;

    ESP_RETURN_ON_ERROR(video_read_header(player), TAG, "video header failed");
    ESP_RETURN_ON_ERROR(video_alloc_buffers(player), TAG, "video buffer allocation failed");

    jpeg_decode_engine_cfg_t dec_cfg = {
        .intr_priority = 0,
        .timeout_ms = -1,
    };
    ESP_RETURN_ON_ERROR(jpeg_new_decoder_engine(&dec_cfg, &player->decoder), TAG, "jpeg decoder init failed");

    ESP_RETURN_ON_ERROR(video_decode_frame(player, 0, player->slots[0].buf), TAG, "first frame decode failed");
    player->slots[0].state = VIDEO_SLOT_DISPLAYING;
    player->displayed_slot = 0;
    player->next_seq = 1;
    player->next_frame_id = 1;
    player->running = true;

    ESP_RETURN_ON_ERROR(video_bind_image(player, image, 0), TAG, "bind lvgl image failed");

    BaseType_t ok = xTaskCreatePinnedToCore(video_decode_task, "video_decode", VIDEO_DECODE_TASK_STACK,
                                            player, VIDEO_DECODE_TASK_PRIO, &player->decode_task_handle, 1);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "create video decode task failed");

    uint32_t frame_period_ms = (uint32_t)(((uint64_t)player->header.fps_den * 1000ULL) / player->header.fps_num);
    if (frame_period_ms == 0) {
        frame_period_ms = 1;
    }
    player->display_timer = lv_timer_create(video_display_timer_cb, frame_period_ms, player);
    ESP_RETURN_ON_FALSE(player->display_timer, ESP_ERR_NO_MEM, TAG, "create display timer failed");

    ok = xTaskCreatePinnedToCore(video_stats_task, "video_stats", 4096,
                                 player, 2, &player->stats_task_handle, 0);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "create video stats task failed");
    return ESP_OK;
}

esp_err_t video_player_stop(void)
{
    video_player_t *player = &s_video;

    if (!player->running && !player->decoder && !player->jpg_buf && !player->index) {
        return ESP_OK;
    }

    player->running = false;

    if (player->display_timer) {
        if (lvgl_port_lock(pdMS_TO_TICKS(1000))) {
            lv_timer_del(player->display_timer);
            player->display_timer = NULL;
            if (player->image) {
                lv_image_set_src(player->image, NULL);
            }
            lvgl_port_unlock();
        } else {
            return ESP_ERR_TIMEOUT;
        }
    }

    esp_err_t decode_wait_ret = video_wait_task_exit(&player->decode_task_handle);
    esp_err_t stats_wait_ret = video_wait_task_exit(&player->stats_task_handle);

    if (player->decoder) {
        ESP_RETURN_ON_ERROR(jpeg_del_decoder_engine(player->decoder), TAG, "delete jpeg decoder failed");
        player->decoder = NULL;
    }

    video_free_buffers(player);
    if (player->file) {
        fclose(player->file);
        player->file = NULL;
    }
    player->image = NULL;
    player->frame_buf_size = 0;
    player->displayed_slot = -1;
    player->next_frame_id = 0;
    player->next_seq = 0;
    player->decoded_frames = 0;
    player->displayed_frames = 0;
    player->decode_waits = 0;
    player->display_misses = 0;
    player->display_timer_ticks = 0;

    if (decode_wait_ret != ESP_OK || stats_wait_ret != ESP_OK) {
        return ESP_ERR_TIMEOUT;
    }

    return ESP_OK;
}

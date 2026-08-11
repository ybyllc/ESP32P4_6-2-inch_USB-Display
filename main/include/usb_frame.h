/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint16_t width;
    uint16_t height;
    uint32_t received;
    uint32_t total;
} frame_info_t;

typedef struct {
    size_t data_buffer_len;                   /**< Max data length supported by this frame buffer*/
    size_t data_len;                          /**< Data length of currently store frame */
    uint8_t *data;                            /**< Frame data */
    frame_info_t info;
} frame_t;

esp_err_t frame_allocate(int nb_of_fb, size_t fb_size);

void frame_deallocate(void);

bool frame_pool_ready(void);

void frame_reset(frame_t *frame);

esp_err_t frame_return_empty(frame_t *frame);

esp_err_t frame_send_filled(frame_t *frame);

esp_err_t frame_add_data(frame_t *frame, const uint8_t *data, size_t data_len);

frame_t *frame_get_empty(void);

frame_t *frame_get_filled(void);

frame_t *frame_get_latest_filled(size_t *dropped_count);

frame_t *frame_get_latest_filled_timeout(size_t *dropped_count, TickType_t ticks_to_wait);

size_t frame_empty_count(void);

size_t frame_filled_count(void);

#ifdef __cplusplus
}
#endif

#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define UDISP_WIDTH 480
#define UDISP_HEIGHT 800
#define UDISP_FRAME_LIMIT 300000
#define UDISP_HEADER_SIZE 16
typedef struct {
    uint8_t *data;
    uint32_t capacity;
    uint32_t length;
} udisp_frame_t;
typedef struct {
    void *context;
    udisp_frame_t *(*acquire)(void *context);
    void (*complete)(void *context, udisp_frame_t *frame);
    void (*release)(void *context, udisp_frame_t *frame);
} udisp_pool_t;
typedef struct {
    udisp_pool_t pool;
    udisp_frame_t *frame;
    uint32_t remaining;
    uint32_t accepted, dropped, invalid;
} udisp_parser_t;
void udisp_parser_reset(udisp_parser_t *parser);
void udisp_parser_packet(udisp_parser_t *parser, const uint8_t *data, size_t length);

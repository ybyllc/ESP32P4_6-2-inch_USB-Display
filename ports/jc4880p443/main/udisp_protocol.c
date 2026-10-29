#include "udisp_protocol.h"
#include <string.h>
static uint16_t read16(const uint8_t *p) { return p[0] | ((uint16_t)p[1] << 8); }
static uint32_t read32(const uint8_t *p) {
    return read16(p) | ((uint32_t)read16(p+2) << 16);
}
void udisp_parser_reset(udisp_parser_t *p) {
    if (p->frame) p->pool.release(p->pool.context, p->frame);
    p->frame = NULL;
    p->remaining = 0;
}
void udisp_parser_packet(udisp_parser_t *p, const uint8_t *data, size_t length) {
    if (!length) return;
    if (!p->remaining) {
        if (length < UDISP_HEADER_SIZE) { ++p->invalid; return; }
        /* 作者的packed结构末字：低10位frame_id，高22位payload_total，显式小端解包。 */
        uint32_t bytes = read32(data+12) >> 10;
        if (data[2] == 0xff) return;
        bool valid = data[2] == 3 && read16(data+4) == 0 && read16(data+6) == 0 &&
                     read16(data+8) == UDISP_WIDTH && read16(data+10) == UDISP_HEIGHT &&
                     bytes >= 4 && bytes <= UDISP_FRAME_LIMIT;
        p->remaining = bytes;
        if (valid) {
            p->frame = p->pool.acquire(p->pool.context);
            if (p->frame && p->frame->capacity < bytes) {
                p->pool.release(p->pool.context, p->frame); p->frame = NULL;
            }
        } else ++p->invalid;
        if (!p->frame) ++p->dropped;
        else p->frame->length = 0;
        data += UDISP_HEADER_SIZE; length -= UDISP_HEADER_SIZE;
    }
    /* 不支持的格式或队列满也必须吃完该帧，不能把JPEG片段误认作下一帧头。 */
    if (length > p->remaining) {
        ++p->invalid; udisp_parser_reset(p); return;
    }
    if (p->frame && length) {
        memcpy(p->frame->data + p->frame->length, data, length);
        p->frame->length += length;
    }
    p->remaining -= length;
    if (!p->remaining && p->frame) {
        udisp_frame_t *done = p->frame; p->frame = NULL;
        if (done->data[0] != 0xff || done->data[1] != 0xd8 ||
            done->data[done->length-2] != 0xff || done->data[done->length-1] != 0xd9) {
            ++p->invalid; p->pool.release(p->pool.context, done); return;
        }
        ++p->accepted; p->pool.complete(p->pool.context, done);
    }
}

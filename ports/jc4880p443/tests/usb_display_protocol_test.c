#include "udisp_protocol.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
typedef struct { unsigned taken, completed, released; bool busy; uint8_t bytes[32]; udisp_frame_t frame; } pool_t;
static udisp_frame_t *take(void *context) {
    pool_t *pool=context;
    if(pool->busy) return NULL;
    pool->busy=true; ++pool->taken; return &pool->frame;
}
static void done(void *context, udisp_frame_t *frame) {
    pool_t *pool=context; assert(frame==&pool->frame); ++pool->completed; pool->busy=false;
}
static void put(void *context, udisp_frame_t *frame) {
    pool_t *pool=context; assert(frame==&pool->frame); ++pool->released; pool->busy=false;
}
static void header(uint8_t *p, uint8_t type, uint32_t size) {
    memset(p,0,16);p[2]=type;p[8]=480&255;p[9]=480>>8;p[10]=800&255;p[11]=800>>8;
    uint32_t word=(size<<10)|1023;
    for(unsigned i=0;i<4;++i)p[12+i]=(uint8_t)(word>>(8*i));
}
int main(void) {
    pool_t pool={0};pool.frame=(udisp_frame_t){.data=pool.bytes,.capacity=sizeof(pool.bytes)};
    udisp_parser_t p={.pool={.context=&pool,.acquire=take,.complete=done,.release=put}};
    uint8_t packet[32];
    /* Given合法JPEG跨USB包；When逐包送入；Then恰好完成一次且内容完整。 */
    header(packet,3,6);packet[16]=0xff;packet[17]=0xd8;
    udisp_parser_packet(&p,packet,18);
    const uint8_t tail[]={0x12,0x34,0xff,0xd9};udisp_parser_packet(&p,tail,4);
    assert(pool.completed==1 && p.accepted==1 && pool.frame.length==6 && pool.bytes[2]==0x12);
    /* Given格式不支持；When其payload跨包；Then完整跳过、不借用缓冲、不误识别JPEG片段。 */
    header(packet,0,6);udisp_parser_packet(&p,packet,18);udisp_parser_packet(&p,tail,4);
    assert(pool.taken==1 && p.remaining==0 && p.dropped==1);
    /* GivenJPEG头里的几何尺寸错误；When收完整帧；Then拒绝并保持帧边界。 */
    header(packet,3,4);packet[8]=1;memcpy(packet+16,"abcd",4);udisp_parser_packet(&p,packet,20);
    assert(pool.completed==1 && p.remaining==0);
    /* Given包长超过声明；When到达；Then释放借用帧，不越界写。 */
    header(packet,3,4);memset(packet+16,0xaa,5);udisp_parser_packet(&p,packet,21);
    assert(pool.released==1 && !pool.busy && p.remaining==0);
    /* Given断线发生在半帧；When reset；Then只释放一次，随后可以接收新帧。 */
    header(packet,3,6);udisp_parser_packet(&p,packet,16);udisp_parser_reset(&p);udisp_parser_reset(&p);
    assert(pool.released==2 && !pool.busy);
    header(packet,3,4);packet[16]=0xff;packet[17]=0xd8;packet[18]=0xff;packet[19]=0xd9;
    udisp_parser_packet(&p,packet,20);assert(pool.completed==2);
    /* Given输入缓冲不足或全部在用；When新帧；Then跳过而非覆盖借用帧。 */
    pool.busy=true;header(packet,3,4);udisp_parser_packet(&p,packet,20);assert(pool.completed==2);
    pool.busy=false;header(packet,3,33);udisp_parser_packet(&p,packet,16);assert(!pool.busy && !p.frame);
    uint8_t discard[33]={0};udisp_parser_packet(&p,discard,33);assert(p.remaining==0);
    puts("PASS: fragmented JPEG, invalid geometry/type, overflow, disconnect, exhausted pool and reuse");
}

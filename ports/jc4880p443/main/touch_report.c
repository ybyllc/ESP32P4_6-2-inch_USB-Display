#include "touch_report.h"
#include "udisp_protocol.h"
#include <string.h>
void usb_display_touch_report(uint8_t report[USB_DISPLAY_TOUCH_REPORT_BYTES],
                              bool pressed, uint16_t x, uint16_t y) {
    memset(report, 0, USB_DISPLAY_TOUCH_REPORT_BYTES);
    x = x >= UDISP_WIDTH ? UDISP_WIDTH-1 : x;
    y = y >= UDISP_HEIGHT ? UDISP_HEIGHT-1 : y;
    /* 抬手仍带最后位置及同一Contact ID，避免把释放误表现为跳到左上角。 */
    report[0] = pressed ? 1 : 0;
    report[1] = 0;
    report[2] = x & 255; report[3] = x >> 8;
    report[4] = y & 255; report[5] = y >> 8;
    report[6] = 4; report[7] = 0;
    report[8] = 4; report[9] = 0;
    report[50] = pressed ? 1 : 0;
}

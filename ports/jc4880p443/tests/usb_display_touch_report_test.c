#include "touch_report.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
int main(void) {
    struct { uint8_t before, report[USB_DISPLAY_TOUCH_REPORT_BYTES], after; } guarded;
    memset(&guarded, 0xa5, sizeof(guarded));
    /* Given有效坐标；When按下；Then第一指小端坐标正确，触点数1，其他指槽全0。 */
    usb_display_touch_report(guarded.report, true, 321, 654);
    assert(guarded.before == 0xa5 && guarded.after == 0xa5);
    assert(USB_DISPLAY_TOUCH_REPORT_BYTES == 51);
    assert(guarded.report[0] == 1 && guarded.report[1] == 0 && guarded.report[50] == 1);
    assert(guarded.report[2] == 65 && guarded.report[3] == 1);
    assert(guarded.report[4] == 142 && guarded.report[5] == 2);
    assert(guarded.report[6] == 4 && guarded.report[7] == 0);
    assert(guarded.report[8] == 4 && guarded.report[9] == 0);
    for(unsigned i=10;i<50;++i) assert(guarded.report[i] == 0);
    /* Given越界传感器数据；When封报告；Then只截到479/799，不回绕。 */
    usb_display_touch_report(guarded.report, true, 65535, 65535);
    assert((guarded.report[2] | (guarded.report[3]<<8)) == 479);
    assert((guarded.report[4] | (guarded.report[5]<<8)) == 799);
    /* Given抬手；When发送release；Then保留最后位置和ID，Tip及触点数均0。 */
    usb_display_touch_report(guarded.report, false, 321, 654);
    assert(guarded.report[0] == 0 && guarded.report[1] == 0 && guarded.report[50] == 0);
    assert(guarded.report[2] == 65 && guarded.report[3] == 1);
    assert(guarded.report[4] == 142 && guarded.report[5] == 2);
    assert(guarded.before == 0xa5 && guarded.after == 0xa5);
    puts("PASS: HID press/move/release, coordinates, boundaries, unused contacts and buffer guards");
}

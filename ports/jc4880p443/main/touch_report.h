#pragma once
#include <stdbool.h>
#include <stdint.h>
/* 与固定上游描述符一致：五个10字节手指槽和一个Contact Count；宽/高也是16位。 */
#define USB_DISPLAY_TOUCH_REPORT_BYTES 51
void usb_display_touch_report(uint8_t report[USB_DISPLAY_TOUCH_REPORT_BYTES],
                              bool pressed, uint16_t x, uint16_t y);

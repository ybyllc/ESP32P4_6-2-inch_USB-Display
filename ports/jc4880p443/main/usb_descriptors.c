/* 协议/字符串格式与panyihang固定版本兼容；触摸报告定义复用其MIT授权头文件。 */
#include "tusb.h"
#include "usb_descriptors.h"
#include <string.h>
#ifndef UDISP_USB_PID
/* 已安装的Windows INF：2987绑定整个设备，2986&MI_00仅绑定显示接口。
 * 使用2986让Usbccgp拆分vendor/HID；不能让显示驱动吞掉触摸接口。 */
#define UDISP_USB_PID 0x2986
#endif
static tusb_desc_device_t const device = {
    .bLength=sizeof(tusb_desc_device_t), .bDescriptorType=TUSB_DESC_DEVICE, .bcdUSB=0x0200,
    .bMaxPacketSize0=64, .idVendor=0x303a, .idProduct=UDISP_USB_PID,
    .bcdDevice=0x0202, .iManufacturer=1, .iProduct=2, .iSerialNumber=3, .bNumConfigurations=1,
};
static uint8_t const report[] = {TUD_HID_REPORT_DESC_TOUCH_SCREEN(REPORT_ID_TOUCH, 480, 800)};
#define TOTAL (TUD_CONFIG_DESC_LEN + TUD_VENDOR_DESC_LEN + TUD_HID_DESC_LEN)
#define CONFIGURATION(packet) \
    TUD_CONFIG_DESCRIPTOR(1, 2, 0, TOTAL, 0, 250), \
    TUD_VENDOR_DESCRIPTOR(0, 4, 0x01, 0x81, packet), \
    TUD_HID_DESCRIPTOR(1, 5, HID_ITF_PROTOCOL_NONE, sizeof(report), 0x82, 64, 10)
static const uint8_t high_speed[] = {CONFIGURATION(512)};
static const uint8_t full_speed[] = {CONFIGURATION(64)};
static uint8_t other_speed[TOTAL];
static const tusb_desc_device_qualifier_t qualifier = {
    .bLength=sizeof(tusb_desc_device_qualifier_t), .bDescriptorType=TUSB_DESC_DEVICE_QUALIFIER,
    .bcdUSB=0x0200, .bMaxPacketSize0=64, .bNumConfigurations=1,
};
uint8_t const *tud_descriptor_device_cb(void) { return (const uint8_t *)&device; }
uint8_t const *tud_descriptor_configuration_cb(uint8_t index) {
    (void)index; return tud_speed_get() == TUSB_SPEED_HIGH ? high_speed : full_speed;
}
uint8_t const *tud_descriptor_other_speed_configuration_cb(uint8_t index) {
    (void)index;
    memcpy(other_speed, tud_speed_get() == TUSB_SPEED_HIGH ? full_speed : high_speed, TOTAL);
    other_speed[1]=TUSB_DESC_OTHER_SPEED_CONFIG; return other_speed;
}
uint8_t const *tud_descriptor_device_qualifier_cb(void) { return (const uint8_t *)&qualifier; }
uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance) { (void)instance; return report; }
uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
    (void)langid;
    static uint16_t text[64];
    /* Windows驱动读取iProduct，Linux移植读取vendor接口字符串；两处都必须报告配置。
     * Windows Bl的单位是KiB，292KiB低于设备300000字节上限；Linux接口保留作者字节口径。 */
    static const char *const strings[] = {"", "Espressif", "esp32p4udisp0_R480x800_Ejpg6_Fps30_Bl292", "JC4880-USB-01",
        "esp32p4udisp0_R480x800_Ejpg6_Fps30_Bl300000", "touch"};
    if (!index) { text[0]=(TUSB_DESC_STRING<<8)|4; text[1]=0x0409; return text; }
    if (index >= sizeof(strings)/sizeof(strings[0])) return NULL;
    size_t n = strlen(strings[index]); if (n > 63) n=63;
    for (size_t i=0;i<n;++i) text[i+1]=(uint8_t)strings[index][i];
    text[0]=(TUSB_DESC_STRING<<8)|(2*n+2); return text;
}

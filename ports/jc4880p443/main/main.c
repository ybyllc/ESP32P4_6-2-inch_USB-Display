#include "board.h"
#include "receiver.h"
#include "touch_report.h"
#include "esp_check.h"
#include "esp_private/usb_phy.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "tusb.h"
#include "usb_descriptors.h"
#include <string.h>
static usb_phy_handle_t phy;
static esp_lcd_touch_handle_t touch;
static void usb_task(void *arg) { (void)arg; for (;;) tud_task(); }
static void touch_task(void *arg) {
    (void)arg;
    uint8_t report[USB_DISPLAY_TOUCH_REPORT_BYTES];
    bool was_pressed = false;
    uint16_t last_x = 0, last_y = 0;
    unsigned read_errors = 0, transitions = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(10));
        if (!tud_hid_ready() || !touch) continue;
        esp_err_t result = esp_lcd_touch_read_data(touch);
        if (result != ESP_OK) {
            if (++read_errors <= 3) ESP_LOGW("usb_touch", "GT911 read failed: %s", esp_err_to_name(result));
            continue;
        }
        uint16_t x, y, strength;
        uint8_t count = 0;
        bool pressed = esp_lcd_touch_get_coordinates(touch, &x, &y, &strength, &count, 1) && count > 0;
        if (!pressed && !was_pressed) continue;
        if (pressed) { last_x=x; last_y=y; }
        usb_display_touch_report(report, pressed, last_x, last_y);
        if (tud_hid_report(REPORT_ID_TOUCH, report, sizeof(report))) {
            // 只记录前几次按下/抬手，不逐点打印、不拖慢移动回传。
            if (pressed != was_pressed && ++transitions <= 10)
                ESP_LOGI("usb_touch", "HID pressed=%u", (unsigned)pressed);
            was_pressed = pressed;
        }
    }
}
uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t type,
                             uint8_t *buffer, uint16_t length) {
    (void)instance; (void)type;
    if (report_id == REPORT_ID_MAX_COUNT && length) { buffer[0]=1; return 1; }
    return 0;
}
void tud_hid_set_report_cb(uint8_t instance, uint8_t id, hid_report_type_t type,
                         uint8_t const *buffer, uint16_t size) {
    (void)instance; (void)id; (void)type; (void)buffer; (void)size;
}
void app_main(void) {
    esp_lcd_panel_handle_t panel;
    void *first, *second;
    ESP_ERROR_CHECK(usb_display_board_init(&panel, &first, &second, &touch));
    ESP_LOGI("usb_touch", "GT911 available=%u max_contacts=1", (unsigned)(touch != NULL));
    ESP_ERROR_CHECK(usb_display_receiver_init(panel, first, second));
    usb_phy_config_t config = {.controller=USB_PHY_CTRL_OTG, .target=USB_PHY_TARGET_INT,
                               .otg_mode=USB_OTG_MODE_DEVICE, .otg_speed=USB_PHY_SPEED_HIGH};
    ESP_ERROR_CHECK(usb_new_phy(&config, &phy));
    ESP_ERROR_CHECK(tusb_init() ? ESP_OK : ESP_FAIL);
    ESP_ERROR_CHECK(xTaskCreatePinnedToCore(usb_task, "usb_device", 4096, NULL, 7, NULL, 0) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_ERROR_CHECK(xTaskCreatePinnedToCore(touch_task, "usb_touch", 3072, NULL, 3, NULL, 0) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_LOGI("usb_display", "READY JC4880 480x800 USB-HS vendor+HID; no phone resources mounted");
}

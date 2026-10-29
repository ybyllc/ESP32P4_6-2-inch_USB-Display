#pragma once
#include "esp_err.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_touch.h"
esp_err_t usb_display_board_init(esp_lcd_panel_handle_t *panel, void **first, void **second,
                                 esp_lcd_touch_handle_t *touch);

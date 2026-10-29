#pragma once
#include "esp_err.h"
#include "esp_lcd_panel_ops.h"
esp_err_t usb_display_receiver_init(esp_lcd_panel_handle_t panel, void *first, void *second);

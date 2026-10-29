get_filename_component(upstream "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
idf_component_register(SRCS main.c board.c receiver.c udisp_protocol.c touch_report.c
    INCLUDE_DIRS "." "${upstream}/main/tusb"
    PRIV_REQUIRES esp_lcd esp_driver_gpio esp_driver_i2c esp_driver_ledc esp_driver_jpeg
                  esp_hw_support esp_mm esp_timer esp_psram esp_lcd_st7701
                  espressif__esp_lcd_touch_gt911 espressif__tinyusb)
idf_component_get_property(tusb_lib espressif__tinyusb COMPONENT_LIB)
target_include_directories(${tusb_lib} PUBLIC "${CMAKE_CURRENT_LIST_DIR}")
target_include_directories(${tusb_lib} PRIVATE "${upstream}/main/tusb")
# TinyUSB回调从它自己的归档解析，避免仅含描述符的main对象被静态链接器丢弃。
target_sources(${tusb_lib} PRIVATE "${CMAKE_CURRENT_LIST_DIR}/usb_descriptors.c")

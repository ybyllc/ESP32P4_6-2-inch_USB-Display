set(srcs "app_assets.c" "app_hid.c" "app_msc.c" "app_usb.c" "app_usb_health.c" "app_vendor.c"
         "app_status_led.c" "app_lcd_ui.c" "video_player.c"
         "usb_extend_screen.c" "usb_frame.c" "simhei_50.c" "simhei_30.c")

if(CONFIG_HID_TOUCH_ENABLE)
    list (APPEND srcs "app_touch.c")
endif()

if(CONFIG_IDF_TARGET_ESP32P4)
    list(APPEND srcs "app_lcd_p4.c")
endif()

idf_component_register(SRCS ${srcs}
                       INCLUDE_DIRS "include" "tusb"
                       PRIV_REQUIRES esp_lcd_axs15260
                                     esp_lcd
                                     esp_driver_gpio
                                     esp_driver_ledc
                                     esp_driver_ppa
                                     esp_lvgl_port
                                     esp_driver_jpeg
                                     esp_partition
                                     nvs_flash
                                     fatfs
                                     esp_hw_support
                                     esp_mm
                                     led_strip
                                     esp_timer
                                     soc
                                     wear_levelling
                                     usb)

idf_component_get_property(esp_lcd_dir esp_lcd COMPONENT_DIR)
target_include_directories(${COMPONENT_LIB} PRIVATE "${esp_lcd_dir}/priv_include")

# Determine whether tinyusb is fetched from component registry or from local path
idf_build_get_property(build_components BUILD_COMPONENTS)
if(tinyusb IN_LIST build_components)
    set(tinyusb_name tinyusb) # Local component
else()
    set(tinyusb_name espressif__tinyusb) # Managed component
endif()

idf_component_get_property(tusb_lib ${tinyusb_name} COMPONENT_LIB)
target_include_directories(${tusb_lib} PUBLIC "${PROJECT_DIR}/main/tusb")
target_sources(${tusb_lib} PUBLIC "${PROJECT_DIR}/main/tusb/usb_descriptors.c")

fatfs_create_spiflash_image(assets "${PROJECT_DIR}/assets" FLASH_IN_PROJECT)

cmake_policy(SET CMP0079 NEW)
target_link_libraries(${tusb_lib} PRIVATE ${COMPONENT_LIB})

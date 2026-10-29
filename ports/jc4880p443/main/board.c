#include "board.h"
#include "driver/i2c_master.h"
#include "driver/ledc.h"
#include "esp_check.h"
#include "esp_ldo_regulator.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_st7701.h"
#include "esp_lcd_touch_gt911.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
static const char *TAG = "usb_board";
static esp_ldo_channel_handle_t mipi_power;
static i2c_master_bus_handle_t touch_bus;
static esp_lcd_touch_io_gt911_config_t touch_address;

esp_err_t usb_display_board_init(esp_lcd_panel_handle_t *panel, void **first, void **second,
                                 esp_lcd_touch_handle_t *touch) {
    /* 严格沿用本机已点亮的屏参，不使用作者452×1280屏的初始化命令。 */
    esp_ldo_channel_config_t power = {.chan_id=3, .voltage_mv=2500};
    ESP_RETURN_ON_ERROR(esp_ldo_acquire_channel(&power, &mipi_power), TAG, "MIPI power");
    esp_lcd_dsi_bus_handle_t bus;
    /* 厂商1.1.3宏仍用旧P4的PLL_F20M默认值；rev3.x的IDF6必须改用新PLL参考时钟。 */
    esp_lcd_dsi_bus_config_t bus_config = {
        .bus_id=0, .num_data_lanes=2, .phy_clk_src=MIPI_DSI_PHY_PLLREF_CLK_SRC_DEFAULT,
        .lane_bit_rate_mbps=1000,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_dsi_bus(&bus_config, &bus), TAG, "DSI bus");
    esp_lcd_dbi_io_config_t dbi = ST7701_PANEL_IO_DBI_CONFIG();
    esp_lcd_panel_io_handle_t io;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_dbi(bus, &dbi, &io), TAG, "DBI");
    esp_lcd_dpi_panel_config_t dpi = {
        .virtual_channel=0, .dpi_clk_src=MIPI_DSI_DPI_CLK_SRC_DEFAULT, .dpi_clock_freq_mhz=28,
        .in_color_format=LCD_COLOR_FMT_RGB565, .out_color_format=LCD_COLOR_FMT_RGB565, .num_fbs=2,
        .video_timing={.h_size=480, .v_size=800, .hsync_back_porch=42, .hsync_pulse_width=12,
                       .hsync_front_porch=42, .vsync_back_porch=8, .vsync_pulse_width=2, .vsync_front_porch=166},
    };
    st7701_vendor_config_t vendor = {.flags={.use_mipi_interface=1},
                                     .mipi_config={.dsi_bus=bus, .dpi_config=&dpi}};
    esp_lcd_panel_dev_config_t config = {.reset_gpio_num=5, .rgb_ele_order=LCD_RGB_ELEMENT_ORDER_RGB,
        .data_endian=LCD_RGB_DATA_ENDIAN_BIG, .bits_per_pixel=16, .vendor_config=&vendor};
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st7701(io, &config, panel), TAG, "ST7701");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(*panel), TAG, "reset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(*panel), TAG, "init");
    ESP_RETURN_ON_ERROR(esp_lcd_dpi_panel_get_frame_buffer(*panel, 2, first, second), TAG, "frame buffers");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(*panel, true), TAG, "LCD on");
    ledc_timer_config_t timer = {.speed_mode=LEDC_LOW_SPEED_MODE, .duty_resolution=LEDC_TIMER_13_BIT,
                                .timer_num=LEDC_TIMER_1, .freq_hz=1000, .clk_cfg=LEDC_AUTO_CLK};
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer), TAG, "backlight timer");
    ledc_channel_config_t led = {.gpio_num=23, .speed_mode=LEDC_LOW_SPEED_MODE,
        .channel=LEDC_CHANNEL_1, .timer_sel=LEDC_TIMER_1, .duty=4096};
    ESP_RETURN_ON_ERROR(ledc_channel_config(&led), TAG, "backlight");
    i2c_master_bus_config_t i2c = {.i2c_port=0, .sda_io_num=7, .scl_io_num=8,
        .clk_source=I2C_CLK_SRC_DEFAULT, .flags={.enable_internal_pullup=1}};
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&i2c, &touch_bus), TAG, "touch I2C");
    esp_lcd_panel_io_i2c_config_t touch_io = ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG();
    touch_io.dev_addr = i2c_master_probe(touch_bus, 0x5d, 100) == ESP_OK ? 0x5d : 0x14;
    esp_lcd_panel_io_handle_t tp_io;
    esp_err_t ret = esp_lcd_new_panel_io_i2c(touch_bus, &touch_io, &tp_io);
    if (ret != ESP_OK) { *touch = NULL; ESP_LOGW(TAG, "touch IO unavailable"); return ESP_OK; }
    touch_address.dev_addr = touch_io.dev_addr;
    esp_lcd_touch_config_t tp = {.x_max=480, .y_max=800, .rst_gpio_num=-1, .int_gpio_num=-1,
                                  .driver_data=&touch_address};
    /* 触摸失败保留副屏显示；不能因可选输入硬件让整机启动失败。 */
    ret = esp_lcd_touch_new_i2c_gt911(tp_io, &tp, touch);
    if (ret != ESP_OK) { *touch = NULL; ESP_LOGW(TAG, "touch unavailable: %s", esp_err_to_name(ret)); }
    return ESP_OK;
}

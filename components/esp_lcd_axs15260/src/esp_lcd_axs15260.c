
#include <stdlib.h>
#include <string.h>
#include <sys/cdefs.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_check.h"
#include "esp_lcd_panel_interface.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_commands.h"
#include "esp_lcd_mipi_dsi.h"
#include "driver/gpio.h"

#include "esp_lcd_axs15260.h"

static const char *TAG = "axs15260";


static const uint8_t cmd_bb_unlock[] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x5a, 0xa5};
static const uint8_t cmd_f8[] = {0x21, 0xA0};
static const uint8_t cmd_a0[] = {
    0x00, 0x10, 0x2C, 0x02, 0x00, 0x00, 0x09, 0xFF,
    0x00, 0x05, 0x3a, 0x3a, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x0E};
static const uint8_t cmd_a1[] = {
    0x8f, 0xE5, 0x11, 0xaa, 0x55, 0x00, 0x02, 0x00,
    0x00, 0x00, 0x01, 0x26, 0x26, 0x32, 0x92, 0x93,
    0x13, 0x92, 0x90, 0x90, 0x90, 0x84};
static const uint8_t cmd_a2[] = {
    0x00, 0x32, 0x0A, 0x0A, 0x5A, 0xFA, 0x5A, 0x00,
    0x00, 0x00, 0x00, 0x00, 0xFF, 0x80, 0x43, 0x88,
    0x88, 0xff, 0xff, 0x20, 0x90, 0x00, 0x20, 0x90,
    0x00, 0xE0, 0x01, 0x7F, 0xFF, 0x00, 0x00, 0x00,
    0x00, 0x00, 0xE7, 0xFF, 0xFF, 0x00};
static const uint8_t cmd_a4[] = {
    0x85, 0x85, 0x92, 0x82, 0xAF, 0xAD, 0xAD, 0x80,
    0x10, 0x30, 0x40, 0x40, 0x20, 0x50, 0x60, 0x53};
static const uint8_t cmd_b8[] = {
    0x03, 0x08, 0x08, 0x20, 0x00, 0x02, 0x50, 0x5e,
    0x1f, 0x8f, 0x40, 0x00, 0x03, 0x00, 0x83, 0x90,
    0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90,
    0x90, 0x90, 0x90, 0x90};
static const uint8_t cmd_b9[] = {
    0x64, 0x34, 0x78, 0x32, 0xAA, 0x55, 0xAA, 0x00,
    0x00, 0x00, 0xF0, 0x00, 0x13, 0xC8, 0x00, 0x10,
    0x27, 0xC8, 0x00, 0x64, 0x10, 0xFF, 0x14, 0x07,
    0x1E, 0x0A, 0x00, 0x00, 0x00, 0x00};
static const uint8_t cmd_ba[] = {
    0x40, 0x80, 0x0E, 0x10, 0x0E, 0x17, 0x90, 0x13,
    0x03, 0xff, 0x04, 0x22, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x30};
static const uint8_t cmd_c1[] = {
    0x72, 0x04, 0x02, 0x02, 0x71, 0x05, 0x18, 0x00,
    0x02, 0x00, 0x01, 0x01, 0x43, 0xff, 0xff, 0x7f,
    0x4f, 0x52, 0x00, 0x4f, 0x52, 0x00, 0x54, 0x3b,
    0x0b, 0x04, 0x06, 0xff, 0xff, 0x00};
static const uint8_t cmd_c3[] = {0x00, 0xc0};
static const uint8_t cmd_c4[] = {
    0x02, 0x02, 0xc0, 0x83, 0x00, 0x63, 0x00, 0x0c,
    0x03, 0x0c, 0x01, 0x01, 0x03, 0x10, 0x3e, 0x06,
    0x9d, 0x05, 0x03, 0x80, 0xfe, 0x10, 0x10, 0x00,
    0x0a, 0x0a, 0x48, 0x48, 0x84, 0xCD};
static const uint8_t cmd_c5[] = {
    0x19, 0x19, 0x00, 0x48, 0x50, 0x48, 0xa0, 0x55,
    0x30, 0x10, 0x88, 0x19, 0x19, 0x19, 0x19, 0x19,
    0x19, 0x6B, 0x03, 0x10, 0x10, 0x10, 0x00};
static const uint8_t cmd_c6[] = {
    0x05, 0x0a, 0x05, 0x0A, 0xc0, 0xe0, 0x2e, 0x03,
    0x12, 0x22, 0x12, 0x22, 0x01, 0x00, 0x00, 0x02,
    0xC8, 0x22, 0xFA, 0xE8, 0x30, 0x64, 0x00, 0x08,
    0x00, 0x09, 0xF0, 0x00, 0x00, 0xF0, 0x01};
static const uint8_t cmd_c7[] = {
    0x50, 0x10, 0x28, 0x00, 0xa2, 0x00, 0x4f, 0x00,
    0x00, 0xFF, 0xa8, 0x99, 0x9C, 0x60, 0x07, 0x04,
    0x0c, 0x0d, 0x0e, 0x0f, 0x01, 0x01, 0x01, 0x01,
    0x30, 0x10, 0x19, 0xff, 0xff, 0xff, 0xff, 0x03,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
static const uint8_t cmd_cf[] = {
    0x3C, 0x1E, 0x88, 0x50, 0xFF, 0x18, 0x16, 0x18,
    0x16, 0x0A, 0x8C, 0x3C, 0x6B, 0x0C, 0x6E, 0x88,
    0x0C, 0x0F, 0x22, 0x88, 0xAA, 0x55, 0x04, 0x04,
    0x91, 0xA0, 0x30, 0x24, 0xBB, 0x01, 0x00};
static const uint8_t cmd_d0[] = {
    0x00, 0x00, 0x01, 0x24, 0x08, 0x05, 0x30, 0x01,
    0xff, 0x11, 0xc3, 0xc2, 0x22, 0x22, 0x00, 0x03,
    0x10, 0x12, 0x40, 0x10, 0x1e, 0x51, 0x15, 0x00,
    0x20, 0x20, 0x00, 0x03, 0x0d, 0x26, 0xa2, 0x28,
    0x28, 0x28, 0x28, 0x28, 0x28, 0x00, 0x3f, 0xff,
    0x0d, 0x02, 0x13, 0x12};
static const uint8_t cmd_d5[] = {
    0x37, 0x3C, 0x93, 0x00, 0x4C, 0x08, 0x6C, 0x74,
    0x00, 0x67, 0x85, 0x0A, 0x08, 0x01, 0x00, 0x4B,
    0x37, 0x3C, 0x37, 0x15, 0x85, 0x01, 0x03, 0x00,
    0x00, 0x55, 0x7B, 0x37, 0x3C, 0x00, 0x37, 0x3C,
    0x04, 0x00, 0x21, 0x5A, 0x1f, 0x30, 0x30};
static const uint8_t cmd_d6[] = {
    0x10, 0x32, 0x54, 0x76, 0x98, 0xBA, 0xDC, 0xFE,
    0x6D, 0x00, 0x01, 0x83, 0x86, 0x66, 0xA0, 0x86,
    0x66, 0xA0, 0x17, 0x3C, 0x1B, 0x3C, 0x37, 0x3C,
    0x00, 0x88, 0x08, 0x28, 0x01, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x12, 0x00, 0x00,
    0x00, 0x00, 0x20};
static const uint8_t cmd_d7[] = {
    0x1B, 0x1C, 0x01, 0x17, 0x15, 0x13, 0x11, 0x0F,
    0x0D, 0x0B, 0x09, 0x19, 0x1A, 0x1F, 0x1F, 0x1F, 0x1F};
static const uint8_t cmd_d8[] = {
    0x1B, 0x18, 0x00, 0x16, 0x14, 0x12, 0x10, 0x0E,
    0x0C, 0x0A, 0x08, 0x19, 0x1A, 0x1F, 0x1F, 0x1F, 0x1F};
static const uint8_t cmd_df[] = {0x00, 0x00, 0x5b, 0xab, 0xbb, 0x2b, 0x28};
// 🎨 Gamma 正向
static const uint8_t cmd_e0[] = {
    0x00, 0x01, 0x03, 0x07, 0x09, 0x0A, 0x0D, 0x0C,
    0x17, 0x2A, 0x3B, 0x3D, 0x4B, 0x61, 0x6C, 0x78,
    0x90, 0xA0, 0xA1, 0xB7, 0xC0, 0x60, 0x5F, 0x63,
    0x68, 0x6C, 0x6E, 0x75, 0x7F, 0x33, 0x35, 0x03};
// 🎨 Gamma 反向
static const uint8_t cmd_e1[] = {
    0x00, 0x01, 0x03, 0x07, 0x09, 0x0A, 0x0D, 0x0C,
    0x17, 0x2A, 0x3B, 0x3D, 0x4B, 0x61, 0x6C, 0x78,
    0x90, 0xA0, 0xA1, 0xB7, 0xC0, 0x60, 0x5F, 0x63,
    0x68, 0x6C, 0x6E, 0x75, 0x7F, 0x33, 0x35, 0xd8, 0x33};
static const uint8_t cmd_e7[] = {
    0x00, 0x05, 0xC4, 0x01, 0x00, 0x05, 0xC4, 0x01,
    0x00, 0x10, 0x00, 0x08, 0xE0, 0x07};
static const uint8_t cmd_e8[] = {
    0xE9, 0x05, 0x00, 0x00, 0x00, 0x00, 0x01, 0x01,
    0x02, 0x30, 0x0D, 0x00, 0xCF, 0x20, 0x00, 0xFF,
    0x40, 0x00, 0x00, 0x00, 0x00, 0x00};
static const uint8_t cmd_e9[] = {
    0x00, 0x2B, 0x02, 0x00, 0x02, 0x03, 0x00, 0xb2,
    0x10, 0x0e, 0x60, 0x14, 0x05, 0x81, 0x01, 0x06,
    0x05, 0x00, 0x80, 0x07, 0x08, 0x07};
// 🔒 锁定命令
static const uint8_t cmd_bb_lock[] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

// ============================================================================
// 📦 内部数据结构
// ============================================================================

/**
 * @brief 🔧 AXS15260 面板上下文
 */
typedef struct
{
    esp_lcd_panel_t base;                     // 📦 基础面板结构
    esp_lcd_panel_io_handle_t io;             // 📡 IO 句柄
    esp_lcd_panel_handle_t dpi_panel;         // 🖥️ DPI 面板句柄
    int reset_gpio_num;                       // 🔌 复位 GPIO
    uint8_t madctl_val;                       // 📝 MADCTL 寄存器值
    uint8_t colmod_val;                       // 🎨 颜色模式值
    const axs15260_lcd_init_cmd_t *init_cmds; // 📋 初始化命令
    uint16_t init_cmds_size;                  // 📋 命令数量
    struct
    {
        unsigned int reset_level : 1;   // 🔧 复位电平
        unsigned int mirror_by_cmd : 1; // 🔧 命令镜像
    } flags;
} axs15260_panel_t;

// ============================================================================
// 🔧 内部函数声明
// ============================================================================
static esp_err_t panel_axs15260_del(esp_lcd_panel_t *panel);
static esp_err_t panel_axs15260_reset(esp_lcd_panel_t *panel);
static esp_err_t panel_axs15260_init(esp_lcd_panel_t *panel);
static esp_err_t panel_axs15260_draw_bitmap(esp_lcd_panel_t *panel, int x_start, int y_start, int x_end, int y_end, const void *color_data);
static esp_err_t panel_axs15260_invert_color(esp_lcd_panel_t *panel, bool invert_color_data);
static esp_err_t panel_axs15260_mirror(esp_lcd_panel_t *panel, bool mirror_x, bool mirror_y);
static esp_err_t panel_axs15260_swap_xy(esp_lcd_panel_t *panel, bool swap_axes);
static esp_err_t panel_axs15260_set_gap(esp_lcd_panel_t *panel, int x_gap, int y_gap);
static esp_err_t panel_axs15260_disp_on_off(esp_lcd_panel_t *panel, bool on_off);
static esp_err_t panel_axs15260_sleep(esp_lcd_panel_t *panel, bool sleep);
static esp_err_t axs15260_tx_param_checked(esp_lcd_panel_io_handle_t io, int cmd, const void *param, size_t param_size, uint32_t delay_ms);
static esp_err_t axs15260_config_reset_gpio(axs15260_panel_t *axs15260);
static esp_err_t axs15260_hw_reset(axs15260_panel_t *axs15260, uint32_t assert_ms, uint32_t release_ms);

// ============================================================================
// 🚀 公共 API 实现
// ============================================================================

/**
 * @brief 📋 发送默认初始化命令序列
 * @note 必须在 DPI 面板创建之前调用
 */
static esp_err_t axs15260_send_init_cmds(esp_lcd_panel_io_handle_t io, uint8_t colmod_val)
{
    ESP_LOGI(TAG, "📋 发送 AXS15260 初始化命令序列...");

    // 🔓 解锁
    ESP_RETURN_ON_ERROR(axs15260_tx_param_checked(io, 0xBB, cmd_bb_unlock, sizeof(cmd_bb_unlock), 5), TAG, "unlock command failed");
    vTaskDelay(1);
    // 📋 发送配置命令
    ESP_RETURN_ON_ERROR(axs15260_tx_param_checked(io, 0xF8, cmd_f8, sizeof(cmd_f8), 2), TAG, "0xF8 failed");
    vTaskDelay(1);
    ESP_RETURN_ON_ERROR(axs15260_tx_param_checked(io, 0xA0, cmd_a0, sizeof(cmd_a0), 2), TAG, "0xA0 failed");
    vTaskDelay(1);
    ESP_RETURN_ON_ERROR(axs15260_tx_param_checked(io, 0xA1, cmd_a1, sizeof(cmd_a1), 2), TAG, "0xA1 failed");
    vTaskDelay(1);
    ESP_RETURN_ON_ERROR(axs15260_tx_param_checked(io, 0xA2, cmd_a2, sizeof(cmd_a2), 2), TAG, "0xA2 failed");
    vTaskDelay(1);
    ESP_RETURN_ON_ERROR(axs15260_tx_param_checked(io, 0xA4, cmd_a4, sizeof(cmd_a4), 2), TAG, "0xA4 failed");
    vTaskDelay(1);
    ESP_RETURN_ON_ERROR(axs15260_tx_param_checked(io, 0xB8, cmd_b8, sizeof(cmd_b8), 2), TAG, "0xB8 failed");
    vTaskDelay(1);
    ESP_RETURN_ON_ERROR(axs15260_tx_param_checked(io, 0xB9, cmd_b9, sizeof(cmd_b9), 2), TAG, "0xB9 failed");
    vTaskDelay(1);
    ESP_RETURN_ON_ERROR(axs15260_tx_param_checked(io, 0xBA, cmd_ba, sizeof(cmd_ba), 2), TAG, "0xBA failed");
    vTaskDelay(1);
    ESP_RETURN_ON_ERROR(axs15260_tx_param_checked(io, 0xC1, cmd_c1, sizeof(cmd_c1), 2), TAG, "0xC1 failed");
    vTaskDelay(1);
    ESP_RETURN_ON_ERROR(axs15260_tx_param_checked(io, 0xC3, cmd_c3, sizeof(cmd_c3), 2), TAG, "0xC3 failed");
    vTaskDelay(1);
    ESP_RETURN_ON_ERROR(axs15260_tx_param_checked(io, 0xC4, cmd_c4, sizeof(cmd_c4), 2), TAG, "0xC4 failed");
    vTaskDelay(1);
    ESP_RETURN_ON_ERROR(axs15260_tx_param_checked(io, 0xC5, cmd_c5, sizeof(cmd_c5), 2), TAG, "0xC5 failed");
    vTaskDelay(1);
    ESP_RETURN_ON_ERROR(axs15260_tx_param_checked(io, 0xC6, cmd_c6, sizeof(cmd_c6), 2), TAG, "0xC6 failed");
    vTaskDelay(1);
    ESP_RETURN_ON_ERROR(axs15260_tx_param_checked(io, 0xC7, cmd_c7, sizeof(cmd_c7), 2), TAG, "0xC7 failed");
    vTaskDelay(1);
    ESP_RETURN_ON_ERROR(axs15260_tx_param_checked(io, 0xCF, cmd_cf, sizeof(cmd_cf), 2), TAG, "0xCF failed");
    vTaskDelay(1);
    ESP_RETURN_ON_ERROR(axs15260_tx_param_checked(io, 0xD0, cmd_d0, sizeof(cmd_d0), 2), TAG, "0xD0 failed");
    vTaskDelay(1);
    ESP_RETURN_ON_ERROR(axs15260_tx_param_checked(io, 0xD5, cmd_d5, sizeof(cmd_d5), 2), TAG, "0xD5 failed");
    vTaskDelay(1);
    ESP_RETURN_ON_ERROR(axs15260_tx_param_checked(io, 0xD6, cmd_d6, sizeof(cmd_d6), 2), TAG, "0xD6 failed");
    vTaskDelay(1);
    ESP_RETURN_ON_ERROR(axs15260_tx_param_checked(io, 0xD7, cmd_d7, sizeof(cmd_d7), 2), TAG, "0xD7 failed");
    vTaskDelay(1);
    ESP_RETURN_ON_ERROR(axs15260_tx_param_checked(io, 0xD8, cmd_d8, sizeof(cmd_d8), 2), TAG, "0xD8 failed");
    vTaskDelay(1);
    ESP_RETURN_ON_ERROR(axs15260_tx_param_checked(io, 0xDF, cmd_df, sizeof(cmd_df), 2), TAG, "0xDF failed");
    vTaskDelay(1);
    ESP_RETURN_ON_ERROR(axs15260_tx_param_checked(io, 0xE0, cmd_e0, sizeof(cmd_e0), 2), TAG, "0xE0 failed");
    vTaskDelay(1);  
    ESP_RETURN_ON_ERROR(axs15260_tx_param_checked(io, 0xE1, cmd_e1, sizeof(cmd_e1), 2), TAG, "0xE1 failed");
    vTaskDelay(1);
    ESP_RETURN_ON_ERROR(axs15260_tx_param_checked(io, 0xE7, cmd_e7, sizeof(cmd_e7), 2), TAG, "0xE7 failed");
    vTaskDelay(1);
    ESP_RETURN_ON_ERROR(axs15260_tx_param_checked(io, 0xE8, cmd_e8, sizeof(cmd_e8), 2), TAG, "0xE8 failed");
    vTaskDelay(1);
    ESP_RETURN_ON_ERROR(axs15260_tx_param_checked(io, 0xE9, cmd_e9, sizeof(cmd_e9), 2), TAG, "0xE9 failed");
    vTaskDelay(1);
    ESP_RETURN_ON_ERROR(axs15260_tx_param_checked(io, 0x36, (static const uint8_t[]){0x08}, 1, 2), TAG, "0x36 failed");
    vTaskDelay(1);
    ESP_RETURN_ON_ERROR(axs15260_tx_param_checked(io, 0x3a, (static const uint8_t[]){0x77}, 1, 2), TAG, "0x3a failed");
    vTaskDelay(1);
    // 🔒 锁定
    ESP_RETURN_ON_ERROR(axs15260_tx_param_checked(io, 0xBB, cmd_bb_lock, sizeof(cmd_bb_lock), 20), TAG, "lock command failed");
    vTaskDelay(1);

    // 🎨 设置颜色模式
    ESP_RETURN_ON_ERROR(axs15260_tx_param_checked(io, LCD_CMD_COLMOD, &colmod_val, 1, 20), TAG, "COLMOD failed");
    vTaskDelay(1);

    // 🚀 退出睡眠模式
    ESP_RETURN_ON_ERROR(axs15260_tx_param_checked(io, LCD_CMD_SLPOUT, NULL, 0, 150), TAG, "SLPOUT failed");

    // 🖥️ 开启显示
    ESP_RETURN_ON_ERROR(axs15260_tx_param_checked(io, LCD_CMD_DISPON, NULL, 0, 100), TAG, "DISPON failed");

    ESP_LOGI(TAG, "✅ 初始化命令发送完成");
    return ESP_OK;
}

esp_err_t esp_lcd_new_panel_axs15260(const esp_lcd_panel_io_handle_t io,
                                     const esp_lcd_panel_dev_config_t *panel_dev_config,
                                     esp_lcd_panel_handle_t *ret_panel)
{
    ESP_RETURN_ON_FALSE(io && panel_dev_config && ret_panel, ESP_ERR_INVALID_ARG, TAG, "❌ 参数无效");

    axs15260_panel_t *axs15260 = calloc(1, sizeof(axs15260_panel_t));
    ESP_RETURN_ON_FALSE(axs15260, ESP_ERR_NO_MEM, TAG, "❌ 内存分配失败");

    // 🔧 获取厂商配置
    axs15260_vendor_config_t *vendor_config = (axs15260_vendor_config_t *)panel_dev_config->vendor_config;

    // 📋 设置初始化命令
    if (vendor_config && vendor_config->init_cmds && vendor_config->init_cmds_size > 0)
    {
        axs15260->init_cmds = vendor_config->init_cmds;
        axs15260->init_cmds_size = vendor_config->init_cmds_size;
    }
    else
    {
        axs15260->init_cmds = NULL;
        axs15260->init_cmds_size = 0;
    }

    // 🔧 基础配置
    axs15260->io = io;
    axs15260->reset_gpio_num = panel_dev_config->reset_gpio_num;
    axs15260->flags.reset_level = panel_dev_config->flags.reset_active_high;
    esp_err_t ret = axs15260_config_reset_gpio(axs15260);
    if (ret != ESP_OK)
    {
        free(axs15260);
        return ret;
    }

    if (vendor_config)
    {
        axs15260->flags.mirror_by_cmd = vendor_config->flags.mirror_by_cmd;
    }

    // 🎨 设置颜色模式
    switch (panel_dev_config->bits_per_pixel)
    {
    case 16:
        axs15260->colmod_val = 0x55; // RGB565
        break;
    case 18:
        axs15260->colmod_val = 0x66; // RGB666
        break;
    case 24:
    default:
        axs15260->colmod_val = 0x77; // RGB888
        break;
    }

    // 🔧 设置 MADCTL
    axs15260->madctl_val = 0;
    switch (panel_dev_config->rgb_ele_order)
    {
    case LCD_RGB_ELEMENT_ORDER_RGB:
        axs15260->madctl_val &= ~(1 << 3);
        break;
    case LCD_RGB_ELEMENT_ORDER_BGR:
        axs15260->madctl_val |= (1 << 3);
        break;
    default:
        break;
    }

    // 📦 设置面板操作函数
    axs15260->base.del = panel_axs15260_del;
    axs15260->base.reset = panel_axs15260_reset;
    axs15260->base.init = panel_axs15260_init;
    axs15260->base.draw_bitmap = panel_axs15260_draw_bitmap;
    axs15260->base.invert_color = panel_axs15260_invert_color;
    axs15260->base.set_gap = panel_axs15260_set_gap;
    axs15260->base.mirror = panel_axs15260_mirror;
    axs15260->base.swap_xy = panel_axs15260_swap_xy;
    axs15260->base.disp_on_off = panel_axs15260_disp_on_off;
    axs15260->base.disp_sleep = panel_axs15260_sleep;

    // 🖥️ 创建 DPI 面板 (如果使用 MIPI 接口)
    // ⚠️ 重要: 必须在创建 DPI 面板之前发送初始化命令！
    // 因为创建 DPI 面板后，MIPI DSI 会进入 Video Mode，无法再发送 DBI 命令
    if (vendor_config && vendor_config->flags.use_mipi_interface)
    {
        ESP_RETURN_ON_FALSE(vendor_config->mipi_config.dsi_bus, ESP_ERR_INVALID_ARG, TAG, "❌ DSI 总线句柄为空");
        ESP_RETURN_ON_FALSE(vendor_config->mipi_config.dpi_config, ESP_ERR_INVALID_ARG, TAG, "❌ DPI 配置为空");
        ESP_RETURN_ON_ERROR(axs15260_hw_reset(axs15260, 150, 200), TAG, "panel reset before init failed");

        // 📋 在创建 DPI 面板之前发送初始化命令
        if (axs15260->init_cmds && axs15260->init_cmds_size > 0)
        {
            // 🔧 使用自定义命令
            ESP_LOGI(TAG, "📋 发送自定义初始化命令 (%d 条)...", axs15260->init_cmds_size);
            for (uint16_t i = 0; i < axs15260->init_cmds_size; i++)
            {
                const axs15260_lcd_init_cmd_t *cmd = &axs15260->init_cmds[i];
                ret = axs15260_tx_param_checked(io, cmd->cmd, cmd->data, cmd->data_bytes, cmd->delay_ms);
                if (ret != ESP_OK)
                {
                    free(axs15260);
                    return ret;
                }
            }
            ESP_LOGI(TAG, "✅ 自定义初始化命令发送完成");
        }
        else
        {
            // 📋 使用默认初始化序列
            ESP_RETURN_ON_ERROR(axs15260_send_init_cmds(io, axs15260->colmod_val), TAG, "❌ 发送初始化命令失败");
        }

        // 🖥️ 现在创建 DPI 面板 (会进入 Video Mode)
        ESP_RETURN_ON_ERROR(
            esp_lcd_new_panel_dpi(vendor_config->mipi_config.dsi_bus,
                                  vendor_config->mipi_config.dpi_config,
                                  &axs15260->dpi_panel),
            TAG, "❌ DPI 面板创建失败");
        ESP_LOGI(TAG, "🖥️ DPI 面板创建成功");
    }

    *ret_panel = &(axs15260->base);
    ESP_LOGI(TAG, "✅ AXS15260 面板创建成功 (%dx%d)", AXS15260_LCD_H_RES, AXS15260_LCD_V_RES);

    return ESP_OK;
}

esp_lcd_panel_handle_t esp_lcd_axs15260_get_dpi_panel(esp_lcd_panel_handle_t panel)
{
    if (panel == NULL)
    {
        return NULL;
    }
    axs15260_panel_t *axs15260 = __containerof(panel, axs15260_panel_t, base);
    return axs15260->dpi_panel;
}

// ============================================================================
// 🔧 内部函数实现
// ============================================================================

static esp_err_t panel_axs15260_del(esp_lcd_panel_t *panel)
{
    axs15260_panel_t *axs15260 = __containerof(panel, axs15260_panel_t, base);

    // 🗑️ 删除 DPI 面板
    if (axs15260->dpi_panel)
    {
        esp_lcd_panel_del(axs15260->dpi_panel);
    }

    // 🔌 释放复位 GPIO
    if (axs15260->reset_gpio_num >= 0)
    {
        gpio_reset_pin(axs15260->reset_gpio_num);
    }

    free(axs15260);
    ESP_LOGI(TAG, "🗑️ AXS15260 面板已删除");
    return ESP_OK;
}

static esp_err_t axs15260_tx_param_checked(esp_lcd_panel_io_handle_t io, int cmd, const void *param, size_t param_size, uint32_t delay_ms)
{
    esp_err_t ret = esp_lcd_panel_io_tx_param(io, cmd, param, param_size);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "send cmd 0x%02X failed: %s", cmd, esp_err_to_name(ret));
        return ret;
    }

    if (delay_ms > 0)
    {
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }

    return ESP_OK;
}

static esp_err_t axs15260_config_reset_gpio(axs15260_panel_t *axs15260)
{
    if (axs15260->reset_gpio_num < 0)
    {
        return ESP_OK;
    }

    gpio_config_t rst_cfg = {
        .pin_bit_mask = 1ULL << axs15260->reset_gpio_num,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&rst_cfg), TAG, "configure reset gpio failed");

    gpio_set_level(axs15260->reset_gpio_num, !axs15260->flags.reset_level);
    vTaskDelay(pdMS_TO_TICKS(1));
    return ESP_OK;
}

static esp_err_t axs15260_hw_reset(axs15260_panel_t *axs15260, uint32_t assert_ms, uint32_t release_ms)
{
    if (axs15260->reset_gpio_num < 0)
    {
        ESP_LOGW(TAG, "no reset gpio configured, fallback to software reset");
        return axs15260_tx_param_checked(axs15260->io, LCD_CMD_SWRESET, NULL, 0, release_ms);
    }

    ESP_LOGI(TAG, "hardware reset panel before DSI init");
    gpio_set_level(axs15260->reset_gpio_num, axs15260->flags.reset_level);
    vTaskDelay(pdMS_TO_TICKS(assert_ms));
    gpio_set_level(axs15260->reset_gpio_num, !axs15260->flags.reset_level);
    vTaskDelay(pdMS_TO_TICKS(release_ms));
    gpio_set_level(axs15260->reset_gpio_num, axs15260->flags.reset_level);
    vTaskDelay(pdMS_TO_TICKS(assert_ms));
    gpio_set_level(axs15260->reset_gpio_num, !axs15260->flags.reset_level);
    vTaskDelay(pdMS_TO_TICKS(release_ms));
    return ESP_OK;
}

static esp_err_t panel_axs15260_reset(esp_lcd_panel_t *panel)
{
    axs15260_panel_t *axs15260 = __containerof(panel, axs15260_panel_t, base);
    return axs15260_hw_reset(axs15260, 20, 170);
}

static esp_err_t panel_axs15260_init(esp_lcd_panel_t *panel)
{
    axs15260_panel_t *axs15260 = __containerof(panel, axs15260_panel_t, base);

    // 🖥️ 初始化 DPI 面板
    // ⚠️ 注意: 初始化命令已经在 esp_lcd_new_panel_axs15260() 中发送了
    // 因为必须在创建 DPI 面板之前发送（MIPI DSI 进入 Video Mode 后无法发送 DBI 命令）
    if (axs15260->dpi_panel)
    {
        ESP_LOGI(TAG, "🖥️ 初始化 DPI 面板...");
        ESP_RETURN_ON_ERROR(esp_lcd_panel_init(axs15260->dpi_panel), TAG, "❌ DPI 面板初始化失败");
    }

    ESP_LOGI(TAG, "✅ AXS15260 初始化完成");
    return ESP_OK;
}

static esp_err_t panel_axs15260_draw_bitmap(esp_lcd_panel_t *panel, int x_start, int y_start, int x_end, int y_end, const void *color_data)
{
    axs15260_panel_t *axs15260 = __containerof(panel, axs15260_panel_t, base);

    // 🖥️ 使用 DPI 面板绘制
    if (axs15260->dpi_panel)
    {
        return esp_lcd_panel_draw_bitmap(axs15260->dpi_panel, x_start, y_start, x_end, y_end, color_data);
    }

    return ESP_OK;
}

static esp_err_t panel_axs15260_invert_color(esp_lcd_panel_t *panel, bool invert_color_data)
{
    axs15260_panel_t *axs15260 = __containerof(panel, axs15260_panel_t, base);
    int cmd = invert_color_data ? LCD_CMD_INVON : LCD_CMD_INVOFF;
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(axs15260->io, cmd, NULL, 0), TAG, "❌ 发送命令失败");
    return ESP_OK;
}

static esp_err_t panel_axs15260_mirror(esp_lcd_panel_t *panel, bool mirror_x, bool mirror_y)
{
    // ⚠️ MIPI DSI DPI 面板不支持硬件镜像，但返回 ESP_OK 避免 LVGL 报错
    // 如需镜像，请使用 LVGL 软件旋转
    if (mirror_x || mirror_y)
    {
        ESP_LOGW(TAG, "⚠️ mirror 不支持硬件实现，请使用软件旋转");
    }
    return ESP_OK;
}

static esp_err_t panel_axs15260_swap_xy(esp_lcd_panel_t *panel, bool swap_axes)
{
    // ⚠️ MIPI DSI DPI 面板不支持硬件 swap_xy，但返回 ESP_OK 避免 LVGL 报错
    // 如需旋转，请使用 LVGL 软件旋转
    if (swap_axes)
    {
        ESP_LOGW(TAG, "⚠️ swap_xy 不支持硬件实现，请使用软件旋转");
    }
    return ESP_OK;
}

static esp_err_t panel_axs15260_set_gap(esp_lcd_panel_t *panel, int x_gap, int y_gap)
{
    axs15260_panel_t *axs15260 = __containerof(panel, axs15260_panel_t, base);

    if (axs15260->dpi_panel)
    {
        return esp_lcd_panel_set_gap(axs15260->dpi_panel, x_gap, y_gap);
    }

    ESP_LOGW(TAG, "⚠️ set_gap 不支持");
    return ESP_ERR_NOT_SUPPORTED;
}

static esp_err_t panel_axs15260_disp_on_off(esp_lcd_panel_t *panel, bool on_off)
{
    axs15260_panel_t *axs15260 = __containerof(panel, axs15260_panel_t, base);
    int cmd = on_off ? LCD_CMD_DISPON : LCD_CMD_DISPOFF;
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(axs15260->io, cmd, NULL, 0), TAG, "❌ 发送命令失败");
    ESP_LOGI(TAG, "🖥️ 显示 %s", on_off ? "开启" : "关闭");
    return ESP_OK;
}

static esp_err_t panel_axs15260_sleep(esp_lcd_panel_t *panel, bool sleep)
{
    axs15260_panel_t *axs15260 = __containerof(panel, axs15260_panel_t, base);
    int cmd = sleep ? LCD_CMD_SLPIN : LCD_CMD_SLPOUT;
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(axs15260->io, cmd, NULL, 0), TAG, "❌ 发送命令失败");
    vTaskDelay(pdMS_TO_TICKS(sleep ? 5 : 120));
    ESP_LOGI(TAG, "😴 睡眠模式 %s", sleep ? "进入" : "退出");
    return ESP_OK;
}

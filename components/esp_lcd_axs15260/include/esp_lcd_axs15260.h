/**
 * @file esp_lcd_axs15260.h
 * @brief 🖥️ ESP LCD AXS15260 MIPI-DSI 驱动头文件
 * 
 * @note 分辨率: 452x1280, 2 Lane MIPI DSI, 60Hz
 * @note 支持 ESP-IDF v5.3 及以上版本
 * 
 * SPDX-FileCopyrightText: 2025
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdint.h>
#include "esp_err.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_mipi_dsi.h"

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// 📐 LCD 分辨率配置
// ============================================================================
#define AXS15260_LCD_H_RES      452     // 🔧 水平分辨率
#define AXS15260_LCD_V_RES      1280    // 🔧 垂直分辨率

// ============================================================================
// ⏱️ 时序参数配置 (来自厂家初始化文件)
// 📋 参考: MIPI 2lane_452x1280_LV_15260D+CTC6.198_60Hz_TP=3ms_gamma2.4_20250313.txt
// ============================================================================
#define AXS15260_HBP            90      // 🔧 Horizontal Back Porch
#define AXS15260_HFP            90      // 🔧 Horizontal Front Porch
#define AXS15260_HSW            10      // 🔧 Horizontal Sync Width
#define AXS15260_VBP            10      // 🔧 Vertical Back Porch
#define AXS15260_VFP            250     // 🔧 Vertical Front Porch
#define AXS15260_VSW            50      // 🔧 Vertical Sync Width

// ============================================================================
// 📡 MIPI DSI 配置
// ============================================================================
#define AXS15260_MIPI_LANES     2       // 🔧 MIPI 数据通道数
#define AXS15260_DCLK_MHZ       48      // 🔧 像素时钟 (DPI Clock) MHz
#define AXS15260_HSCLK_MBPS     1100     // 🔧 高速时钟 (HS Clock) Mbps, lightly tuned down from 920Mbps

// ============================================================================
// 🎨 预定义配置宏
// ============================================================================

/**
 * @brief 🔧 AXS15260 MIPI DSI 总线配置 (2 Lane)
 */
#define AXS15260_PANEL_BUS_DSI_2CH_CONFIG()         \
    {                                               \
        .bus_id = 0,                                \
        .num_data_lanes = AXS15260_MIPI_LANES,      \
        .phy_clk_src = MIPI_DSI_PHY_CLK_SRC_DEFAULT,\
        .lane_bit_rate_mbps = AXS15260_HSCLK_MBPS,  \
    }

/**
 * @brief 🔧 AXS15260 MIPI DBI IO 配置
 */
#define AXS15260_PANEL_IO_DBI_CONFIG()              \
    {                                               \
        .virtual_channel = 0,                       \
        .lcd_cmd_bits = 8,                          \
        .lcd_param_bits = 8,                        \
    }

/**
 * @brief 🔧 AXS15260 452x1280 60Hz DPI 面板配置
 * @param px_format 像素格式 (如 LCD_COLOR_PIXEL_FORMAT_RGB888)
 */
#define AXS15260_452_1280_PANEL_60HZ_DPI_CONFIG(px_format)  \
    {                                                       \
        .dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT,        \
        .dpi_clock_freq_mhz = AXS15260_DCLK_MHZ,            \
        .virtual_channel = 0,                               \
        .pixel_format = px_format,                          \
        .num_fbs = 1,                                       \
        .video_timing = {                                   \
            .h_size = AXS15260_LCD_H_RES,                   \
            .v_size = AXS15260_LCD_V_RES,                   \
            .hsync_back_porch = AXS15260_HBP,               \
            .hsync_pulse_width = AXS15260_HSW,              \
            .hsync_front_porch = AXS15260_HFP,              \
            .vsync_back_porch = AXS15260_VBP,               \
            .vsync_pulse_width = AXS15260_VSW,              \
            .vsync_front_porch = AXS15260_VFP,              \
        },                                                  \
        .flags = {                                          \
            .use_dma2d = true,                              \
        },                                                  \
    }

// ============================================================================
// 📋 初始化命令结构体
// ============================================================================

/**
 * @brief 🔧 AXS15260 LCD 初始化命令结构体
 */
typedef struct {
    uint8_t cmd;            // 📝 命令字节
    uint8_t data[64];       // 📝 数据字节数组
    uint8_t data_bytes;     // 📝 数据字节数
    uint16_t delay_ms;      // ⏱️ 命令后延时 (毫秒)
} axs15260_lcd_init_cmd_t;

// ============================================================================
// 🔧 厂商配置结构体
// ============================================================================

/**
 * @brief 🔧 AXS15260 MIPI 配置
 */
typedef struct {
    esp_lcd_dsi_bus_handle_t dsi_bus;           // 📡 MIPI DSI 总线句柄
    const esp_lcd_dpi_panel_config_t *dpi_config; // 🖥️ DPI 面板配置
    uint8_t lane_num;                           // 📡 数据通道数 (默认 2)
} axs15260_mipi_config_t;

/**
 * @brief 🔧 AXS15260 厂商配置
 */
typedef struct {
    axs15260_mipi_config_t mipi_config;         // 📡 MIPI 配置
    const axs15260_lcd_init_cmd_t *init_cmds;   // 📋 自定义初始化命令 (可选)
    uint16_t init_cmds_size;                    // 📋 初始化命令数量
    struct {
        unsigned int use_mipi_interface: 1;     // 🔧 使用 MIPI 接口
        unsigned int mirror_by_cmd: 1;          // 🔧 通过命令镜像 (而非 LCD 控制器)
    } flags;
} axs15260_vendor_config_t;

// ============================================================================
// 🚀 API 函数
// ============================================================================

/**
 * @brief 🔧 创建 AXS15260 LCD 面板
 *
 * @param[in] io LCD 面板 IO 句柄
 * @param[in] panel_dev_config 面板设备配置
 * @param[out] ret_panel 返回的面板句柄
 * @return
 *      - ESP_OK: ✅ 成功
 *      - ESP_ERR_INVALID_ARG: ❌ 参数无效
 *      - ESP_ERR_NO_MEM: ❌ 内存不足
 *      - ESP_FAIL: ❌ 其他错误
 */
esp_err_t esp_lcd_new_panel_axs15260(const esp_lcd_panel_io_handle_t io,
                                      const esp_lcd_panel_dev_config_t *panel_dev_config,
                                      esp_lcd_panel_handle_t *ret_panel);

/**
 * @brief 🔧 获取 AXS15260 内部的 DPI 面板句柄
 * @note 用于 LVGL 等需要直接访问 DPI 面板的场景
 *
 * @param[in] panel AXS15260 面板句柄
 * @return DPI 面板句柄，如果不存在则返回 NULL
 */
esp_lcd_panel_handle_t esp_lcd_axs15260_get_dpi_panel(esp_lcd_panel_handle_t panel);

#ifdef __cplusplus
}
#endif

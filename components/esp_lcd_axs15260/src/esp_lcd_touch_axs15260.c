/**
 * @file esp_lcd_touch_axs15260.c
 * @brief 👆 AXS15260 触摸屏驱动实现
 * 
 * @note 基于 AXS15260 Linux 驱动 V2.2.4 移植
 * @note I2C 从机地址: 0x3B
 * @note 最大支持 5 点触控
 * 
 * SPDX-FileCopyrightText: 2025
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_check.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"

#include "esp_lcd_touch_axs15260.h"

static const char *TAG = "axs15260_touch";

// ============================================================================
// 📋 寄存器定义
// ============================================================================
#define REG_VERSION             0x0C    // 📖 固件版本寄存器

// ============================================================================
// 📦 内部数据结构
// ============================================================================

struct axs15260_touch_dev {
    i2c_master_bus_handle_t i2c_bus;    // 📡 I2C 总线句柄
    i2c_master_dev_handle_t i2c_dev;    // 📡 I2C 设备句柄
    gpio_num_t rst_gpio;                // 🔄 复位引脚
    gpio_num_t int_gpio;                // ⚡ 中断引脚
    uint16_t x_max;                     // 📐 X 最大值
    uint16_t y_max;                     // 📐 Y 最大值
    uint8_t buf[AXS15260_TOUCH_BUF_SIZE]; // 📦 数据缓冲区
    axs15260_touch_cb_t callback;       // ⚡ 中断回调
    void *user_data;                    // 📦 用户数据
    SemaphoreHandle_t lock;             // 🔒 互斥锁
    struct {
        uint8_t swap_xy: 1;
        uint8_t mirror_x: 1;
        uint8_t mirror_y: 1;
        uint8_t inited: 1;
    } flags;
};

// ============================================================================
// 🔧 内部函数
// ============================================================================

static void IRAM_ATTR touch_isr(void *arg)
{
    axs15260_touch_handle_t handle = (axs15260_touch_handle_t)arg;
    if (handle && handle->callback) {
        handle->callback(handle, handle->user_data);
    }
}

static esp_err_t touch_i2c_read(axs15260_touch_handle_t handle, uint8_t *data, size_t len)
{
    return i2c_master_receive(handle->i2c_dev, data, len, 100);
}

static esp_err_t touch_i2c_write_read(axs15260_touch_handle_t handle,
                                       uint8_t *cmd, size_t cmd_len,
                                       uint8_t *data, size_t data_len)
{
    return i2c_master_transmit_receive(handle->i2c_dev, cmd, cmd_len, data, data_len, 100);
}

// ============================================================================
// 🚀 公共 API 实现
// ============================================================================

esp_err_t axs15260_touch_new(const axs15260_touch_config_t *config, 
                              axs15260_touch_handle_t *handle)
{
    ESP_RETURN_ON_FALSE(config && handle, ESP_ERR_INVALID_ARG, TAG, "❌ 参数无效");

    ESP_LOGI(TAG, "🔧 创建 AXS15260 触摸屏驱动...");

    // 📦 分配内存
    axs15260_touch_handle_t dev = calloc(1, sizeof(struct axs15260_touch_dev));
    ESP_RETURN_ON_FALSE(dev, ESP_ERR_NO_MEM, TAG, "❌ 内存分配失败");

    // 🔧 保存配置
    dev->rst_gpio = config->rst_gpio;
    dev->int_gpio = config->int_gpio;
    dev->x_max = config->x_max > 0 ? config->x_max : AXS15260_TOUCH_H_RES;
    dev->y_max = config->y_max > 0 ? config->y_max : AXS15260_TOUCH_V_RES;
    dev->flags.swap_xy = config->flags.swap_xy;
    dev->flags.mirror_x = config->flags.mirror_x;
    dev->flags.mirror_y = config->flags.mirror_y;

    // 🔒 创建互斥锁
    dev->lock = xSemaphoreCreateMutex();
    if (!dev->lock) {
        ESP_LOGE(TAG, "❌ 创建互斥锁失败");
        free(dev);
        return ESP_ERR_NO_MEM;
    }

    // 🔄 配置复位引脚
    if (config->rst_gpio >= 0) {
        ESP_LOGI(TAG, "🔌 配置复位引脚 (GPIO %d)...", config->rst_gpio);
        gpio_config_t rst_conf = {
            .pin_bit_mask = (1ULL << config->rst_gpio),
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&rst_conf);
        gpio_set_level(config->rst_gpio, 1);
    }

    // ⚡ 配置中断引脚
    if (config->int_gpio >= 0) {
        ESP_LOGI(TAG, "🔌 配置中断引脚 (GPIO %d)...", config->int_gpio);
        gpio_config_t int_conf = {
            .pin_bit_mask = (1ULL << config->int_gpio),
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_NEGEDGE,
        };
        gpio_config(&int_conf);
        gpio_install_isr_service(0);
    }

    // 📡 初始化 I2C
    uint32_t freq = config->i2c_freq_hz > 0 ? config->i2c_freq_hz : AXS15260_TOUCH_I2C_FREQ_HZ;
    ESP_LOGI(TAG, "📡 初始化 I2C (SDA=%d, SCL=%d, 频率=%luHz)...",
             config->i2c_sda, config->i2c_scl, (unsigned long)freq);

    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = config->i2c_port,
        .sda_io_num = config->i2c_sda,
        .scl_io_num = config->i2c_scl,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };

    esp_err_t ret = i2c_new_master_bus(&bus_cfg, &dev->i2c_bus);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "❌ 创建 I2C 总线失败");
        vSemaphoreDelete(dev->lock);
        free(dev);
        return ret;
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = AXS15260_TOUCH_I2C_ADDR,
        .scl_speed_hz = freq,
    };

    ret = i2c_master_bus_add_device(dev->i2c_bus, &dev_cfg, &dev->i2c_dev);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "❌ 添加 I2C 设备失败");
        i2c_del_master_bus(dev->i2c_bus);
        vSemaphoreDelete(dev->lock);
        free(dev);
        return ret;
    }

    ESP_LOGI(TAG, "✅ I2C 初始化完成 (地址: 0x%02X)", AXS15260_TOUCH_I2C_ADDR);
    ret = axs15260_touch_reset(dev);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "⚠️ hardware reset failed, continue with touch probe");
    }

    // 📖 读取固件版本验证通信
    uint16_t version = 0;
    ret = axs15260_touch_get_version(dev, &version);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "⚠️ version probe failed, retrying after touch reset");
        ret = axs15260_touch_reset(dev);
        if (ret == ESP_OK) {
            ret = axs15260_touch_get_version(dev, &version);
        }
    }
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "✅ 固件版本: 0x%04X", version);
    } else {
        ESP_LOGW(TAG, "⚠️ 读取固件版本失败");
    }

    dev->flags.inited = 1;
    *handle = dev;

    ESP_LOGI(TAG, "✅ AXS15260 触摸屏驱动创建成功 (分辨率: %dx%d)", dev->x_max, dev->y_max);
    return ESP_OK;
}

esp_err_t axs15260_touch_del(axs15260_touch_handle_t handle)
{
    ESP_RETURN_ON_FALSE(handle, ESP_ERR_INVALID_ARG, TAG, "❌ 句柄无效");

    ESP_LOGI(TAG, "🗑️ 删除 AXS15260 触摸屏驱动...");

    // ⚡ 移除中断
    if (handle->int_gpio >= 0) {
        gpio_isr_handler_remove(handle->int_gpio);
        gpio_reset_pin(handle->int_gpio);
    }

    // 🔌 释放复位引脚
    if (handle->rst_gpio >= 0) {
        gpio_reset_pin(handle->rst_gpio);
    }

    // 📡 释放 I2C
    if (handle->i2c_dev) {
        i2c_master_bus_rm_device(handle->i2c_dev);
    }
    if (handle->i2c_bus) {
        i2c_del_master_bus(handle->i2c_bus);
    }

    // 🔒 删除互斥锁
    if (handle->lock) {
        vSemaphoreDelete(handle->lock);
    }

    free(handle);
    ESP_LOGI(TAG, "✅ 触摸屏驱动已删除");
    return ESP_OK;
}

esp_err_t axs15260_touch_reset(axs15260_touch_handle_t handle)
{
    ESP_RETURN_ON_FALSE(handle, ESP_ERR_INVALID_ARG, TAG, "❌ 句柄无效");

    if (handle->rst_gpio < 0) {
        ESP_LOGD(TAG, "🔍 未配置复位引脚，跳过复位");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "🔄 执行硬件复位...");

    // 🔄 复位时序: 高 → 低 → 高
    gpio_set_level(handle->rst_gpio, 1);
    vTaskDelay(pdMS_TO_TICKS(5));
    gpio_set_level(handle->rst_gpio, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(handle->rst_gpio, 1);
    vTaskDelay(pdMS_TO_TICKS(150));

    ESP_LOGI(TAG, "✅ 硬件复位完成");
    return ESP_OK;
}

esp_err_t axs15260_touch_read(axs15260_touch_handle_t handle, axs15260_touch_data_t *data)
{
    ESP_RETURN_ON_FALSE(handle && data, ESP_ERR_INVALID_ARG, TAG, "❌ 参数无效");

    // 🔒 获取互斥锁
    if (xSemaphoreTake(handle->lock, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    // 📖 读取触摸数据
    memset(handle->buf, 0xFF, AXS15260_TOUCH_BUF_SIZE);
    esp_err_t ret = touch_i2c_read(handle, handle->buf, AXS15260_TOUCH_BUF_SIZE);
    if (ret != ESP_OK) {
        xSemaphoreGive(handle->lock);
        return ret;
    }

    uint8_t *buf = handle->buf;

    // 🔍 数据有效性校验
    // 正常数据: buf[0]=手势ID(通常0x00), buf[1]低4位=触摸点数(0-5)
    uint8_t gesture = buf[0];
    uint8_t point_byte = buf[1];
    uint8_t point_num = point_byte & 0x0F;
    
    // 📋 校验数据格式
    // 1. 手势ID应该是 0x00-0x0F 范围
    // 2. 触摸点数应该是 0-5
    // 3. buf[1] 高4位是状态标志，不应该有异常值
    if (gesture > 0x0F || point_num > AXS15260_TOUCH_MAX_POINTS) {
        // ⚠️ 数据无效，可能是 I2C 通信错误
        data->point_num = 0;
        data->gesture_id = 0;
        xSemaphoreGive(handle->lock);
        return ESP_OK;
    }

    // 🤚 获取手势 ID
    data->gesture_id = gesture;

    //  检查 ESD 标志 (高 4 位)
    uint8_t esd_flag = point_byte >> 4;
    if (esd_flag && esd_flag != 0x08 && esd_flag != 0x04) {
        // ⚠️ 真正的 ESD 事件，忽略此次数据
        data->point_num = 0;
        xSemaphoreGive(handle->lock);
        return ESP_OK;
    }

    // 📋 限制触摸点数量
    if (point_num > AXS15260_TOUCH_MAX_POINTS) {
        point_num = AXS15260_TOUCH_MAX_POINTS;
    }
    data->point_num = point_num;

    // 📍 解析触摸点 (只解析第一个点，因为缓冲区只有 8 字节)
    if (point_num > 0) {
        uint16_t x = ((buf[2] & 0x0F) << 8) | buf[3];
        uint16_t y = ((buf[4] & 0x0F) << 8) | buf[5];

        // 🔄 坐标变换
        if (handle->flags.swap_xy) {
            uint16_t tmp = x; x = y; y = tmp;
        }
        if (handle->flags.mirror_x) {
            x = handle->x_max - 1 - x;
        }
        if (handle->flags.mirror_y) {
            y = handle->y_max - 1 - y;
        }

        data->points[0].x = x;
        data->points[0].y = y;
        data->points[0].event = buf[2] >> 6;
        data->points[0].id = buf[4] >> 4;
        data->points[0].weight = buf[6];
        data->points[0].area = buf[7] >> 4;
    }

    xSemaphoreGive(handle->lock);
    return ESP_OK;
}

esp_err_t axs15260_touch_get_version(axs15260_touch_handle_t handle, uint16_t *version)
{
    ESP_RETURN_ON_FALSE(handle && version, ESP_ERR_INVALID_ARG, TAG, "❌ 参数无效");

    uint8_t cmd = REG_VERSION;
    uint8_t data[2] = {0};

    esp_err_t ret = touch_i2c_write_read(handle, &cmd, 1, data, 2);
    if (ret != ESP_OK) {
        return ret;
    }

    *version = (data[0] << 8) | data[1];
    return ESP_OK;
}

esp_err_t axs15260_touch_register_cb(axs15260_touch_handle_t handle,
                                      axs15260_touch_cb_t callback,
                                      void *user_data)
{
    ESP_RETURN_ON_FALSE(handle, ESP_ERR_INVALID_ARG, TAG, "❌ 句柄无效");

    handle->callback = callback;
    handle->user_data = user_data;

    if (handle->int_gpio >= 0 && callback) {
        ESP_LOGI(TAG, "⚡ 启用触摸中断 (GPIO %d)", handle->int_gpio);
        gpio_isr_handler_add(handle->int_gpio, touch_isr, handle);
        gpio_intr_enable(handle->int_gpio);
    }

    return ESP_OK;
}

esp_err_t axs15260_touch_set_swap_xy(axs15260_touch_handle_t handle,
                                      bool swap_xy, bool mirror_x, bool mirror_y)
{
    ESP_RETURN_ON_FALSE(handle, ESP_ERR_INVALID_ARG, TAG, "❌ 句柄无效");

    handle->flags.swap_xy = swap_xy;
    handle->flags.mirror_x = mirror_x;
    handle->flags.mirror_y = mirror_y;

    ESP_LOGI(TAG, "🔧 坐标变换: swap_xy=%d, mirror_x=%d, mirror_y=%d",
             swap_xy, mirror_x, mirror_y);
    return ESP_OK;
}

bool axs15260_touch_is_pressed(axs15260_touch_handle_t handle)
{
    if (!handle || handle->int_gpio < 0) {
        return false;
    }
    // 📋 中断引脚低电平表示有触摸
    return gpio_get_level(handle->int_gpio) == 0;
}

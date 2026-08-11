/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Start the USB host liveness monitor.
 *
 * The monitor uses the DWC2 SOF frame counter (DSTS.SOFFN) instead of video
 * traffic: a connected host always sends Start-of-Frame packets, even when
 * the Windows display driver has nothing new to transmit.
 *
 * @return ESP_OK on success, ESP_ERR_NO_MEM if the monitor task cannot be created.
 */
esp_err_t app_usb_health_init(void);

/**
 * @brief Check whether a USB host is currently present and the bus is alive.
 *
 * Returns true while SOF is flowing, or while a mounted session is still
 * inside the short absence debounce window. Returns false when the host is
 * gone (unplugged, power-only, or suspended beyond the debounce).
 */
bool app_usb_host_alive(void);

#ifdef __cplusplus
}
#endif

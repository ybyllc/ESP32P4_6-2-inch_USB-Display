#pragma once

#include "esp_err.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t video_player_start(lv_obj_t *image);
esp_err_t video_player_stop(void);

#ifdef __cplusplus
}
#endif

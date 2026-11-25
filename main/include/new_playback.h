#pragma once

#include <stdint.h>

#include "drv8912.h"
#include "esp_err.h"

esp_err_t new_playback(const char *path,
                       drv8912_handle_t driver,
                       int16_t target_track,
                       int16_t target_channel);

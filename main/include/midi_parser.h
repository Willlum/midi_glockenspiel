#pragma once

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#include "esp_err.h"

esp_err_t play_midi_buffer(const uint8_t *midiBuffer, size_t bufferSize, int16_t targetTrack, int16_t targetChannel);
void read_midi_file(const char *path, int16_t trackNum, int16_t channelNum);

#ifdef __cplusplus
}
#endif
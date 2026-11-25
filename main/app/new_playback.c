#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "new_playback.h"
#include "midi_parser.h"
#include "solenoid.h"

static const char *TAG = "NEW_PLAYBACK";

esp_err_t new_playback(const char *path,
                       drv8912_handle_t driver,
                       int16_t target_track,
                       int16_t target_channel)
{
    if (path == NULL || driver == NULL || target_track < -1 ||
        target_channel < -1 || target_channel > 15) {
        return ESP_ERR_INVALID_ARG;
    }

    struct stat file_stat;
    if (stat(path, &file_stat) != 0) {
        ESP_LOGE(TAG, "Cannot stat MIDI file %s", path);
        return ESP_ERR_NOT_FOUND;
    }
    if (!S_ISREG(file_stat.st_mode) || file_stat.st_size <= 0 ||
        (uintmax_t)file_stat.st_size > SIZE_MAX) {
        ESP_LOGE(TAG, "Invalid MIDI file size for %s", path);
        return ESP_ERR_INVALID_SIZE;
    }

    const size_t file_size = (size_t)file_stat.st_size;
    uint8_t *buffer = heap_caps_malloc(file_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buffer == NULL) {
        buffer = heap_caps_malloc(file_size, MALLOC_CAP_8BIT);
    }
    if (buffer == NULL) {
        ESP_LOGE(TAG, "Not enough memory to load %s (%u bytes)", path, (unsigned)file_size);
        return ESP_ERR_NO_MEM;
    }

    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        ESP_LOGE(TAG, "Cannot open MIDI file %s", path);
        heap_caps_free(buffer);
        return ESP_ERR_NOT_FOUND;
    }

    const size_t bytes_read = fread(buffer, 1, file_size, file);
    const int close_result = fclose(file);
    if (bytes_read != file_size || close_result != 0) {
        ESP_LOGE(TAG, "Failed to read MIDI file %s", path);
        heap_caps_free(buffer);
        return ESP_FAIL;
    }

    esp_err_t err = solenoid_init_drv(driver);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Playing %s", path);
        err = play_midi_buffer(buffer, file_size, target_track, target_channel);
        solenoid_stop();
    }

    heap_caps_free(buffer);
    return err;
}

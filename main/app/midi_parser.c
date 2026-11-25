#include <stdbool.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_midi_define.h"
#include "midi_parser.h"
#include "solenoid.h"

static const char *TAG = "MIDI_PLAYBACK";

static uint32_t current_tempo_us;
static uint16_t time_division;

typedef struct {
    const uint8_t *ptr;
    const uint8_t *end;
    uint32_t absolute_ticks;
    uint8_t running_status;
    bool active;
} track_state_t;

static bool read_be16(const uint8_t **ptr, const uint8_t *end, uint16_t *value)
{
    if ((size_t)(end - *ptr) < 2) {
        return false;
    }

    *value = ((uint16_t)(*ptr)[0] << 8) | (*ptr)[1];
    *ptr += 2;
    return true;
}

static bool read_be32(const uint8_t **ptr, const uint8_t *end, uint32_t *value)
{
    if ((size_t)(end - *ptr) < 4) {
        return false;
    }

    *value = ((uint32_t)(*ptr)[0] << 24) |
             ((uint32_t)(*ptr)[1] << 16) |
             ((uint32_t)(*ptr)[2] << 8) |
             (*ptr)[3];
    *ptr += 4;
    return true;
}

static bool read_vlq(const uint8_t **ptr, const uint8_t *end, uint32_t *value)
{
    uint32_t parsed = 0;
    for (unsigned int i = 0; i < 4; i++) {
        if (*ptr >= end || parsed > (UINT32_MAX >> 7)) {
            return false;
        }

        const uint8_t byte = *(*ptr)++;
        parsed = (parsed << 7) | (byte & 0x7F);
        if ((byte & 0x80) == 0) {
            *value = parsed;
            return true;
        }
    }
    return false;
}

static void wait_ticks(uint32_t ticks)
{
    uint64_t remaining_us = ((uint64_t)ticks * current_tempo_us) / time_division;
    while (remaining_us > 20000) {
        uint64_t delay_ms = remaining_us / 1000;
        if (delay_ms > 1000) {
            delay_ms = 1000;
        }
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
        remaining_us -= delay_ms * 1000;
    }

    if (remaining_us > 0) {
        esp_rom_delay_us(remaining_us);
    }
}

static esp_err_t parse_track_event(track_state_t *track,
                                   uint16_t track_index,
                                   int16_t target_track,
                                   int16_t target_channel)
{
    if (track->ptr >= track->end) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    uint8_t status = *track->ptr;
    if (status < 0x80) {
        if (track->running_status == 0) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        status = track->running_status;
    } else {
        track->ptr++;
        if (status < 0xF0) {
            track->running_status = status;
        } else {
            track->running_status = 0;
        }
    }

    if (status == 0xFF) {
        if (track->ptr >= track->end) {
            return ESP_ERR_INVALID_RESPONSE;
        }

        const uint8_t meta_type = *track->ptr++;
        uint32_t length;
        if (!read_vlq(&track->ptr, track->end, &length) ||
            length > (uint32_t)(track->end - track->ptr)) {
            return ESP_ERR_INVALID_RESPONSE;
        }

        if (meta_type == ESP_MIDI_META_TYPE_SET_TEMPO && track_index == 0 && length == 3) {
            current_tempo_us = ((uint32_t)track->ptr[0] << 16) |
                               ((uint32_t)track->ptr[1] << 8) |
                               track->ptr[2];
            if (current_tempo_us == 0) {
                return ESP_ERR_INVALID_RESPONSE;
            }
            ESP_LOGI(TAG, "Tempo changed to %" PRIu32 " us/qn", current_tempo_us);
        }

        track->ptr += length;
        if (meta_type == 0x2F) {
            track->active = false;
        }
        return ESP_OK;
    }

    if (status == 0xF0 || status == 0xF7) {
        uint32_t length;
        if (!read_vlq(&track->ptr, track->end, &length) ||
            length > (uint32_t)(track->end - track->ptr)) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        track->ptr += length;
        return ESP_OK;
    }

    if (status >= 0xF0) {
        size_t length;
        switch (status) {
        case 0xF1:
        case 0xF3:
            length = 1;
            break;
        case 0xF2:
            length = 2;
            break;
        case 0xF6:
        case 0xF8:
        case 0xFA:
        case 0xFB:
        case 0xFC:
        case 0xFE:
            length = 0;
            break;
        default:
            return ESP_ERR_NOT_SUPPORTED;
        }
        if (length > (size_t)(track->end - track->ptr)) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        track->ptr += length;
        return ESP_OK;
    }

    const uint8_t event_type = status & 0xF0;
    const uint8_t event_channel = status & 0x0F;
    size_t data_length;
    switch (event_type) {
    case ESP_MIDI_EVENT_TYPE_PROGRAM_CHANGE:
    case ESP_MIDI_EVENT_TYPE_CHANNEL_PRESSURE:
        data_length = 1;
        break;
    case ESP_MIDI_EVENT_TYPE_NOTE_OFF:
    case ESP_MIDI_EVENT_TYPE_NOTE_ON:
    case ESP_MIDI_EVENT_TYPE_KEY_PRESSURE:
    case ESP_MIDI_EVENT_TYPE_CONTROL_CHANGE:
    case ESP_MIDI_EVENT_TYPE_PITCH_BEND:
        data_length = 2;
        break;
    default:
        return ESP_ERR_NOT_SUPPORTED;
    }

    if (data_length > (size_t)(track->end - track->ptr)) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    if (event_type == ESP_MIDI_EVENT_TYPE_NOTE_ON &&
        (target_track == -1 || target_track == track_index) &&
        (target_channel == -1 || target_channel == event_channel) &&
        track->ptr[1] != 0) {
        esp_err_t err = solenoid_strike(track->ptr[0], track->ptr[1]);
        if (err != ESP_OK) {
            return err;
        }
    }

    track->ptr += data_length;
    return ESP_OK;
}

esp_err_t play_midi_buffer(const uint8_t *midi_buffer,
                           size_t buffer_size,
                           int16_t target_track,
                           int16_t target_channel)
{
    if (midi_buffer == NULL || target_track < -1 ||
        target_channel < -1 || target_channel > 15) {
        return ESP_ERR_INVALID_ARG;
    }

    const uint8_t *ptr = midi_buffer;
    const uint8_t *buffer_end = midi_buffer + buffer_size;
    uint32_t chunk_id;
    uint32_t header_size;
    uint16_t format_type;
    uint16_t track_count;
    uint16_t division;

    if (!read_be32(&ptr, buffer_end, &chunk_id) || chunk_id != MTHD_FCC ||
        !read_be32(&ptr, buffer_end, &header_size) || header_size < 6 ||
        header_size > (uint32_t)(buffer_end - ptr)) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    const uint8_t *header_end = ptr + header_size;
    if (!read_be16(&ptr, header_end, &format_type) ||
        !read_be16(&ptr, header_end, &track_count) ||
        !read_be16(&ptr, header_end, &division) ||
        track_count == 0 || division == 0 || (division & 0x8000) != 0) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (format_type > 1 || (format_type == 0 && track_count != 1)) {
        ESP_LOGE(TAG, "Unsupported MIDI format %u with %u tracks", format_type, track_count);
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (target_track >= track_count) {
        return ESP_ERR_INVALID_ARG;
    }
    if (track_count > buffer_size / 8) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    ptr = header_end;
    track_state_t *tracks = calloc(track_count, sizeof(*tracks));
    if (tracks == NULL) {
        return ESP_ERR_NO_MEM;
    }

    uint16_t found_tracks = 0;
    while (ptr < buffer_end && found_tracks < track_count) {
        uint32_t track_length;
        if (!read_be32(&ptr, buffer_end, &chunk_id) ||
            !read_be32(&ptr, buffer_end, &track_length) ||
            track_length > (uint32_t)(buffer_end - ptr)) {
            free(tracks);
            return ESP_ERR_INVALID_RESPONSE;
        }

        const uint8_t *track_end = ptr + track_length;
        if (chunk_id == MTRK_FCC) {
            tracks[found_tracks].ptr = ptr;
            tracks[found_tracks].end = track_end;
            if (ptr < track_end) {
                if (!read_vlq(&tracks[found_tracks].ptr, track_end,
                              &tracks[found_tracks].absolute_ticks)) {
                    free(tracks);
                    return ESP_ERR_INVALID_RESPONSE;
                }
                tracks[found_tracks].active = true;
            }
            found_tracks++;
        }
        ptr = track_end;
    }

    if (found_tracks != track_count) {
        free(tracks);
        return ESP_ERR_INVALID_RESPONSE;
    }

    current_tempo_us = 500000;
    time_division = division;
    ESP_LOGI(TAG, "Playing MIDI format %u, %u tracks, division %u",
             format_type, track_count, division);

    uint32_t current_ticks = 0;
    esp_err_t result = ESP_OK;
    while (true) {
        int selected_track = -1;
        uint32_t next_ticks = UINT32_MAX;
        for (uint16_t i = 0; i < track_count; i++) {
            if (tracks[i].active && tracks[i].absolute_ticks < next_ticks) {
                selected_track = i;
                next_ticks = tracks[i].absolute_ticks;
            }
        }
        if (selected_track < 0) {
            break;
        }
        if (next_ticks < current_ticks) {
            result = ESP_ERR_INVALID_RESPONSE;
            break;
        }

        wait_ticks(next_ticks - current_ticks);
        current_ticks = next_ticks;

        track_state_t *track = &tracks[selected_track];
        result = parse_track_event(track, selected_track, target_track, target_channel);
        if (result != ESP_OK) {
            ESP_LOGE(TAG, "Invalid or unsupported event in MIDI track %d", selected_track);
            break;
        }

        if (track->active) {
            if (track->ptr < track->end) {
                uint32_t delta_ticks;
                if (!read_vlq(&track->ptr, track->end, &delta_ticks) ||
                    delta_ticks > UINT32_MAX - track->absolute_ticks) {
                    result = ESP_ERR_INVALID_RESPONSE;
                    break;
                }
                track->absolute_ticks += delta_ticks;
            } else {
                track->active = false;
            }
        }
    }

    free(tracks);
    solenoid_stop();
    return result;
}

void read_midi_file(const char *path, int16_t track_num, int16_t channel_num)
{
    if (path == NULL) {
        ESP_LOGE(TAG, "MIDI path is null");
        return;
    }

    struct stat file_stat;
    if (stat(path, &file_stat) != 0 || file_stat.st_size <= 0 ||
        (uintmax_t)file_stat.st_size > SIZE_MAX) {
        ESP_LOGE(TAG, "Invalid or unavailable MIDI file %s", path);
        return;
    }

    const size_t file_size = (size_t)file_stat.st_size;
    uint8_t *buffer = heap_caps_malloc(file_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buffer == NULL) {
        buffer = heap_caps_malloc(file_size, MALLOC_CAP_8BIT);
    }
    if (buffer == NULL) {
        ESP_LOGE(TAG, "Not enough memory to load %s", path);
        return;
    }

    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        ESP_LOGE(TAG, "Cannot open MIDI file %s", path);
        heap_caps_free(buffer);
        return;
    }

    const size_t bytes_read = fread(buffer, 1, file_size, file);
    const int close_result = fclose(file);
    if (bytes_read != file_size || close_result != 0) {
        ESP_LOGE(TAG, "Failed to read MIDI file %s", path);
        heap_caps_free(buffer);
        return;
    }

    esp_err_t err = play_midi_buffer(buffer, file_size, track_num, channel_num);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "MIDI playback failed: %s", esp_err_to_name(err));
    }
    heap_caps_free(buffer);
}

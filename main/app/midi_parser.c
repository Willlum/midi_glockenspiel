#include <stdio.h>
#include <sys/stat.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_rom_sys.h"
#include "esp_heap_caps.h"
#include "esp_err.h"

#include "esp_midi_define.h"
#include "solenoid.h"
#include "midi_parser.h"

static const char *TAG1 = "MIDI_PLAYBACK";

// Global timing state
static uint32_t currentTempoUs = 500000; // Default: 500,000 microseconds per quarter note (120 BPM)
static uint16_t timeDivision = 480;      // Default: Ticks per quarter note (updated by header)

// State machine for Format 1
typedef struct {
    const uint8_t *ptr;         // Current position in the track
    const uint8_t *end;         // End of the track
    uint32_t absoluteTicks;     // The exact time this track's next event
    uint8_t runningStatus;      
    bool isActive;              
} TrackState;

/**
 * @brief Helper to read variable length quantities
 */
static uint32_t read_vlq(const uint8_t **ptr) {
    uint32_t parsedValue = 0;
    uint8_t currentByte;
    do {
        currentByte = **ptr;
        (*ptr)++;
        parsedValue = (parsedValue << 7) | (currentByte & 0x7F);
    } while (currentByte & 0x80);
    return parsedValue;
}

/**
 * @brief Precise delay based on MIDI ticks
 */
static void wait_ticks(uint32_t deltaTicks) {
    if (deltaTicks == 0) return;

    uint64_t waitTimeUs = ((uint64_t)deltaTicks * currentTempoUs) / timeDivision;

    if (waitTimeUs > 20000) {
        vTaskDelay(pdMS_TO_TICKS(waitTimeUs / 1000));
    } else {
        esp_rom_delay_us(waitTimeUs);
    }
}

esp_err_t play_midi_buffer(const uint8_t *midiBuffer, size_t bufferSize, int16_t targetTrack, int16_t targetChannel) {
    const uint8_t *readPtr = midiBuffer;
    const uint8_t *bufferEnd = midiBuffer + bufferSize;

    // Parse Header Chunk
    uint32_t chunkId;
    ESP_MIDI_READ_4_BYTES(readPtr, chunkId);
    if (chunkId != MTHD_FCC) {
        ESP_LOGE(TAG1, "Invalid MIDI file: Missing MThd header");
        return ESP_FAIL;
    }
    readPtr += 4; 

    uint32_t headerSize;
    ESP_MIDI_READ_4_BYTES(readPtr, headerSize);
    readPtr += 4; 

    uint16_t formatType, numTracks;
    ESP_MIDI_READ_2_BYTES(readPtr, formatType);   readPtr += 2;
    ESP_MIDI_READ_2_BYTES(readPtr, numTracks);    readPtr += 2;
    ESP_MIDI_READ_2_BYTES(readPtr, timeDivision); readPtr += 2;

    ESP_LOGI(TAG1, "Format: %d, Tracks: %d, Division: %d", formatType, numTracks, timeDivision);

    if (targetTrack != -1 && targetTrack >= numTracks) {
        ESP_LOGE(TAG1, "Requested track %d does not exist. File only has %d tracks.", targetTrack, numTracks);
        return ESP_FAIL;
    }

    // =========================================================================
    // MODE A: FORMAT 1 SYNC PLAYBACK (Track 0 Tempo + Target Track Notes)
    // =========================================================================
    if (formatType == 1 && targetTrack > 0) {
        ESP_LOGI(TAG1, "--- Sync Mode: Track 0 (Tempo) + Track %d (Notes) | Channel Filter: %d ---", targetTrack, targetChannel);

        TrackState track0 = {0};
        TrackState trackN = {0};

        // Scan the file to initialize our two playheads
        const uint8_t *scanPtr = readPtr;
        for (uint16_t i = 0; i < numTracks; i++) {
            if (scanPtr >= bufferEnd) break;
            
            ESP_MIDI_READ_4_BYTES(scanPtr, chunkId); scanPtr += 4;
            uint32_t trackLength;
            ESP_MIDI_READ_4_BYTES(scanPtr, trackLength); scanPtr += 4;

            if (chunkId == MTRK_FCC) {
                if (i == 0) {
                    track0.ptr = scanPtr;
                    track0.end = scanPtr + trackLength;
                    if (track0.ptr < track0.end) {
                        track0.absoluteTicks = read_vlq(&track0.ptr);
                        track0.isActive = true;
                    }
                } else if (i == targetTrack) {
                    trackN.ptr = scanPtr;
                    trackN.end = scanPtr + trackLength;
                    if (trackN.ptr < trackN.end) {
                        trackN.absoluteTicks = read_vlq(&trackN.ptr);
                        trackN.isActive = true;
                    }
                }
            }
            scanPtr += trackLength; // Jump to next track
        }

        uint32_t globalTicks = 0;

        // Playback Loop
        while (track0.isActive || trackN.isActive) {
            TrackState *activeTrack = NULL;

            // Determine which track's next event happens
            if (track0.isActive && (!trackN.isActive || track0.absoluteTicks <= trackN.absoluteTicks)) {
                activeTrack = &track0;
            } else {
                activeTrack = &trackN;
            }

            // Wait for the absolute time to arrive
            uint32_t ticksToWait = activeTrack->absoluteTicks - globalTicks;
            wait_ticks(ticksToWait);
            globalTicks = activeTrack->absoluteTicks;

            // Handle running status for the active track
            uint8_t statusByte = *activeTrack->ptr;
            if (statusByte < 0x80) {
                statusByte = activeTrack->runningStatus;
            } else {
                activeTrack->runningStatus = statusByte;
                activeTrack->ptr++;
            }

            uint8_t eventType = statusByte & 0xF0;
            uint8_t eventChannel = statusByte & 0x0F;

            if (eventType == ESP_MIDI_EVENT_TYPE_NOTE_ON) {
                uint8_t noteNumber = *activeTrack->ptr++;
                uint8_t strikeVelocity = *activeTrack->ptr++;
                
                // Only fire solenoids if this event comes from target track
                if (activeTrack == &trackN && (targetChannel == -1 || eventChannel == targetChannel)) {
                    solenoid_strike(noteNumber, strikeVelocity);
                }
            } 
            else if (eventType == ESP_MIDI_EVENT_TYPE_NOTE_OFF || 
                     eventType == ESP_MIDI_EVENT_TYPE_CONTROL_CHANGE || 
                     eventType == ESP_MIDI_EVENT_TYPE_KEY_PRESSURE || 
                     eventType == ESP_MIDI_EVENT_TYPE_PITCH_BEND) {
                activeTrack->ptr += 2; 
            } 
            else if (eventType == ESP_MIDI_EVENT_TYPE_PROGRAM_CHANGE || 
                     eventType == ESP_MIDI_EVENT_TYPE_CHANNEL_PRESSURE) {
                activeTrack->ptr += 1; 
            } 
            else if (statusByte == ESP_MIDI_EVENT_TYPE_META_EVENT) {
                uint8_t metaType = *activeTrack->ptr++;
                uint32_t metaLength = read_vlq(&activeTrack->ptr);

                if (metaType == ESP_MIDI_META_TYPE_SET_TEMPO && activeTrack == &track0) {
                    currentTempoUs = (activeTrack->ptr[0] << 16) | (activeTrack->ptr[1] << 8) | activeTrack->ptr[2];
                    ESP_LOGI(TAG1, "Tempo Sync: %lu us/qn at tick %lu", currentTempoUs, globalTicks);
                } 
                activeTrack->ptr += metaLength;
                activeTrack->runningStatus = 0; 
            }
            else if (statusByte == 0xF0 || statusByte == 0xF7) {
                uint32_t sysexLength = read_vlq(&activeTrack->ptr);
                activeTrack->ptr += sysexLength;
                activeTrack->runningStatus = 0;
            }

            // Find when this specific track's next event will occur
            if (activeTrack->ptr < activeTrack->end) {
                activeTrack->absoluteTicks += read_vlq(&activeTrack->ptr);
            } else {
                activeTrack->isActive = false;
            }
        }
    } 
    // =========================================================================
    // MODE B: SEQUENTIAL PLAYBACK (Format 0, OR Wildcard Target Track)
    // =========================================================================
    else {
        ESP_LOGI(TAG1, "--- Sequential Mode: Target Track = %d | Channel Filter = %d ---", targetTrack, targetChannel);

        for (uint16_t currentTrack = 0; currentTrack < numTracks; currentTrack++) {
            if (readPtr >= bufferEnd) break;

            ESP_MIDI_READ_4_BYTES(readPtr, chunkId);
            if (chunkId != MTRK_FCC) break;
            readPtr += 4;

            uint32_t trackLength;
            ESP_MIDI_READ_4_BYTES(readPtr, trackLength);
            readPtr += 4;

            const uint8_t *trackEndPtr = readPtr + trackLength;

            // If a specific track was requested and this isn't it, skip it
            if (targetTrack != -1 && currentTrack != targetTrack) {
                readPtr = trackEndPtr; 
                continue;
            }

            uint8_t runningStatus = 0;
            ESP_LOGI(TAG1, "Playing Track %d sequentially...", currentTrack);

            while (readPtr < trackEndPtr && readPtr < bufferEnd) {
                
                uint32_t deltaTicks = read_vlq(&readPtr);
                wait_ticks(deltaTicks);

                uint8_t statusByte = *readPtr;
                if (statusByte < 0x80) {
                    statusByte = runningStatus;
                } else {
                    runningStatus = statusByte;
                    readPtr++;
                }

                uint8_t eventType = statusByte & 0xF0;
                uint8_t eventChannel = statusByte & 0x0F;

                if (eventType == ESP_MIDI_EVENT_TYPE_NOTE_ON) {
                    uint8_t noteNumber = *readPtr++;
                    uint8_t strikeVelocity = *readPtr++;
                    
                    if (targetChannel == -1 || eventChannel == targetChannel) {
                        solenoid_strike(noteNumber, strikeVelocity);
                    }
                } 
                else if (eventType == ESP_MIDI_EVENT_TYPE_NOTE_OFF || 
                         eventType == ESP_MIDI_EVENT_TYPE_CONTROL_CHANGE || 
                         eventType == ESP_MIDI_EVENT_TYPE_KEY_PRESSURE || 
                         eventType == ESP_MIDI_EVENT_TYPE_PITCH_BEND) {
                    readPtr += 2; 
                } 
                else if (eventType == ESP_MIDI_EVENT_TYPE_PROGRAM_CHANGE || 
                         eventType == ESP_MIDI_EVENT_TYPE_CHANNEL_PRESSURE) {
                    readPtr += 1; 
                } 
                else if (statusByte == ESP_MIDI_EVENT_TYPE_META_EVENT) {
                    uint8_t metaType = *readPtr++;
                    uint32_t metaLength = read_vlq(&readPtr);

                    if (metaType == ESP_MIDI_META_TYPE_SET_TEMPO) {
                        currentTempoUs = (readPtr[0] << 16) | (readPtr[1] << 8) | readPtr[2];
                        ESP_LOGI(TAG1, "Tempo Changed: %lu us/qn", currentTempoUs);
                    } 
                    readPtr += metaLength;
                    runningStatus = 0; 
                }
                else if (statusByte == 0xF0 || statusByte == 0xF7) {
                    uint32_t sysexLength = read_vlq(&readPtr);
                    readPtr += sysexLength;
                    runningStatus = 0;
                }
            }
        }
    }
    
    solenoid_stop(); 
    return ESP_OK;
}

void read_midi_file(const char *path, int16_t trackNum, int16_t channelNum) {
    struct stat st;
    if (stat(path, &st) != 0) return;

    // Allocate in PSRAM
    uint8_t *buffer = (uint8_t *)heap_caps_malloc(st.st_size, MALLOC_CAP_SPIRAM);
    if (!buffer) return;

    FILE *f = fopen(path, "rb");
    fread(buffer, 1, st.st_size, f);
    fclose(f);

    //scan_midi_instruments(buffer, st.st_size, trackNum);
    play_midi_buffer(buffer, st.st_size, trackNum, channelNum);

    heap_caps_free(buffer);
}

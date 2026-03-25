#ifndef SOLENOID_H
#define SOLENOID_H

#include <stdint.h>
#include "esp_err.h"

#define MAX_SOLENOIDS 8
#define SOLENOID_PULSE_MS 20
#define MIDI_BASE_NOTE 60  // C4

/**
 * @brief Initialize the timers and GPIOs for the solenoids
 * * @param targetPins Array of GPIO numbers
 * * @param enablePin Switch to enbale/disable hardware
 * @return esp_err_t 
 */
esp_err_t solenoid_init(const uint8_t* targetPins, const uint8_t enablePin);

/**
 * @brief Trigger a solenoid strike based on a MIDI note
 * * @param midi_note The MIDI note number (0-127)
 * @param velocity
 */
void solenoid_strike(uint8_t midiNote, uint8_t velocity);

void solenoid_stop(void);

void solenoid_test(void);

#endif // SOLENOID_H
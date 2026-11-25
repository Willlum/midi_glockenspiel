#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NOTE_MAP_MIN_MIDI_NOTE 77
#define NOTE_MAP_MAX_MIDI_NOTE 108
#define NOTE_MAP_COUNT (NOTE_MAP_MAX_MIDI_NOTE - NOTE_MAP_MIN_MIDI_NOTE + 1)

typedef struct {
    uint8_t device_index;
    uint8_t channel;
    bool high_side;
} note_map_entry_t;

// Device indexes follow the chain order used in main.c: U2=0, U1=1, U3=2.
// Channels are DRV8912 half-bridges 1..12. Each entry also notes its PCB connector pin.
static const note_map_entry_t note_map_entries[NOTE_MAP_COUNT] = {
    {0, 4, true},  // MIDI 77,  F5,  J2-1
    {2, 3, true},  // MIDI 78,  F#5, J1-1
    {0, 6, true},  // MIDI 79,  G5,  J2-2
    {2, 10, true}, // MIDI 80,  G#5, J1-2
    {0, 9, true},  // MIDI 81,  A5,  J2-3
    {2, 11, true}, // MIDI 82,  A#5, J1-3
    {0, 7, true},  // MIDI 83,  B5,  J2-4
    {0, 5, true},  // MIDI 84,  C6,  J2-5
    {2, 12, true}, // MIDI 85,  C#6, J1-4
    {0, 1, true},  // MIDI 86,  D6,  J2-6
    {2, 8, true},  // MIDI 87,  D#6, J1-5
    {0, 8, true},  // MIDI 88,  E6,  J2-7
    {0, 2, true},  // MIDI 89,  F6,  J2-8
    {2, 2, true},  // MIDI 90,  F#6, J1-6
    {0, 11, true}, // MIDI 91,  G6,  J2-9
    {2, 1, true},  // MIDI 92,  G#6, J1-7
    {0, 12, true}, // MIDI 93,  A6,  J2-10
    {2, 5, true},  // MIDI 94,  A#6, J1-8
    {0, 3, true},  // MIDI 95,  B6,  J2-11
    {0, 10, true}, // MIDI 96,  C7,  J2-12
    {2, 7, true},  // MIDI 97,  C#7, J1-9
    {1, 2, true},  // MIDI 98,  D7,  J2-13
    {2, 9, true},  // MIDI 99,  D#7, J1-10
    {1, 8, true},  // MIDI 100, E7,  J2-14
    {1, 12, true}, // MIDI 101, F7,  J2-15
    {2, 6, true},  // MIDI 102, F#7, J1-11
    {1, 11, true}, // MIDI 103, G7,  J2-16
    {2, 4, true},  // MIDI 104, G#7, J1-12
    {1, 10, true}, // MIDI 105, A7,  J2-17
    {1, 4, true},  // MIDI 106, A#7, J2-20
    {1, 3, true},  // MIDI 107, B7,  J2-18
    {1, 6, true},  // MIDI 108, C8,  J2-19
};

static inline const note_map_entry_t *note_map_get(uint8_t midi_note)
{
    if (midi_note < NOTE_MAP_MIN_MIDI_NOTE || midi_note > NOTE_MAP_MAX_MIDI_NOTE) {
        return NULL;
    }

    return &note_map_entries[midi_note - NOTE_MAP_MIN_MIDI_NOTE];
}

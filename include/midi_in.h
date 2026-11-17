#ifndef MIDI_IN_H
#define MIDI_IN_H

#include <stdbool.h>
#include <stdint.h>

// Parser for the bytes that arrive on the DIN input. Pure: no hardware, no
// locks, core 0 only.

typedef enum { MIDI_IN_NOTE_ON, MIDI_IN_NOTE_OFF, MIDI_IN_ALL_OFF } midi_in_kind_t;

typedef struct {
    midi_in_kind_t kind;
    uint8_t channel, note, velocity;
} midi_in_event_t;

typedef struct {
    uint8_t status;     // running status, 0 = none: data bytes are ignored
    uint8_t data[2];
    uint8_t have;       // data bytes of the message in progress
} midi_in_parser_t;

// Data bytes are ignored until the next channel status.
void midi_in_parser_reset(midi_in_parser_t *p);
// Feeds one byte. True when it completed an event, which is written to *out.
// A Note On with velocity 0 is reported as a Note Off. Control change 120 and
// 123 are reported as MIDI_IN_ALL_OFF.
bool midi_in_parse(midi_in_parser_t *p, uint8_t byte, midi_in_event_t *out);

#endif

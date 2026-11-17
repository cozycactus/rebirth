#include "midi_in.h"
#include "sequencer.h"

void midi_in_parser_reset(midi_in_parser_t *p) {
    p->status = 0;
    p->data[0] = 0;
    p->data[1] = 0;
    p->have = 0;
}

// Data bytes a channel message carries: 0xC0..0xDF have one, the rest two.
static uint8_t midi_in_data_length(uint8_t status) {
    return ((status & 0xE0u) == 0xC0u) ? 1u : 2u;
}

bool midi_in_parse(midi_in_parser_t *p, uint8_t byte, midi_in_event_t *out) {
    if (byte >= 0xF8u) {
        // Real-time bytes may come between the bytes of a message and change
        // nothing, Active Sensing included. System Reset starts over.
        if (byte == 0xFFu) midi_in_parser_reset(p);
        return false;
    }
    if (byte >= 0xF0u) {
        // System common: any message in progress is dropped and the running
        // status ends, which also skips the body of a SysEx.
        midi_in_parser_reset(p);
        return false;
    }
    if (byte & 0x80u) {
        p->status = byte;
        p->have = 0;
        return false;
    }
    if (p->status == 0) return false;

    p->data[p->have++] = byte;
    if (p->have < midi_in_data_length(p->status)) return false;
    p->have = 0;    // the status stays: running status

    uint8_t channel = p->status & 0x0Fu;
    switch (p->status & 0xF0u) {
    case 0x80u:
        *out = (midi_in_event_t){MIDI_IN_NOTE_OFF, channel, p->data[0], p->data[1]};
        return true;
    case 0x90u:
        if (p->data[1] == 0) {
            *out = (midi_in_event_t){MIDI_IN_NOTE_OFF, channel, p->data[0], 0};
        } else {
            *out = (midi_in_event_t){MIDI_IN_NOTE_ON, channel, p->data[0], p->data[1]};
        }
        return true;
    case 0xB0u:
        if (p->data[0] == MIDI_CC_ALL_SOUND_OFF || p->data[0] == MIDI_CC_ALL_NOTES_OFF) {
            *out = (midi_in_event_t){MIDI_IN_ALL_OFF, channel, 0, 0};
            return true;
        }
        return false;
    default:
        return false;   // consumed with the right length, produces nothing
    }
}

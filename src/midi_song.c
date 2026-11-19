#include "midi_song.h"

#include "hardware/flash.h"
#include "pico/stdlib.h"
#include "sequencer.h"
#include <string.h>

enum { HEADER_BYTES = 40, EVENT_BYTES = 8, MAX_IRQ_EVENTS = 64 };
static const uint8_t *stream;
static uint32_t count, duration, next_index, start_us;
static uint32_t position;
static uint8_t tracks;
static bool running;
static uint64_t present, active, muted;
static int8_t solo_track;
static uint16_t held[MIDI_SONG_MAX_TRACKS][128];  // each bit is a MIDI channel
static uint16_t held_count[MIDI_SONG_MAX_TRACKS];
static uint8_t gate_owner[NUM_CV_CHANNELS];       // 0 means Gate low, track + 1 otherwise

static inline uint32_t read_u32(const uint8_t *p) {
    return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

void midi_song_reset(void) {
    stream = NULL;
    count = duration = position = 0;
    tracks = 0;
    running = false;
    present = active = muted = 0;
    solo_track = -1;
}

static uint32_t checksum(const uint8_t *bytes, uint32_t size) {
    uint32_t crc = UINT32_MAX;
    for (uint32_t i = 0; i < size; i++) {
        crc ^= i >= 16 && i < 20 ? 0 : bytes[i];
        for (uint8_t bit = 0; bit < 8; bit++) crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320u : 0);
    }
    return ~crc;
}

bool midi_song_mount(void) {
    const uint8_t *p = (const uint8_t *)(XIP_BASE + MIDI_SONG_FLASH_OFFSET);
    midi_song_reset();
    if (memcmp(p, "RBM1", 4) != 0) return false;
    uint32_t n = read_u32(p + 4), length = read_u32(p + 8), last = 0;
    uint8_t ntracks = p[12];
    if (!n || n > (MIDI_SONG_FLASH_BYTES - HEADER_BYTES) / EVENT_BYTES ||
        !length || length > INT32_MAX || !ntracks || ntracks > MIDI_SONG_MAX_TRACKS) return false;
    if (checksum(p, HEADER_BYTES + n * EVENT_BYTES) != read_u32(p + 16) || p[39] != 0) return false;
    const uint8_t *event = p + HEADER_BYTES;
    uint64_t present_tracks = 0;
    for (uint32_t i = 0; i < n; i++, event += EVENT_BYTES) {
        const uint32_t time = read_u32(event);
        const uint8_t track = event[4], status = event[5], a = event[6], b = event[7];
        if (time < last || time > length) return false;
        last = time;
        if (status == MIDI_CLOCK) {
            if (track != UINT8_MAX) return false;
        } else if (track >= ntracks) {
            return false;
        } else if (status >= 0x10 && status < 0x10 + NUM_CV_CHANNELS) {
            if (a > 127 || b > 1) return false;
        } else if (status < 0x80 || status >= 0xf0 || a > 127 || b > 127) {
            return false;
        }
        if (track < ntracks) present_tracks |= UINT64_C(1) << track;
    }
    stream = p + HEADER_BYTES;
    count = n;
    duration = length;
    tracks = ntracks;
    present = present_tracks;
    return true;
}

bool midi_song_loaded(void) { return stream != NULL; }

void midi_song_begin(uint32_t now) {
    next_index = position = 0;
    start_us = now;
    memset(held, 0, sizeof(held));
    memset(held_count, 0, sizeof(held_count));
    memset(gate_owner, 0, sizeof(gate_owner));
    active = 0;
    running = true;
}

void midi_song_end(void) {
    if (!running) return;
    running = false;
    active = 0;
    cv_gate_all_off();
    midi_send_all_notes_off();
}

static inline bool enabled(uint8_t track) {
    return solo_track >= 0 ? track == (uint8_t)solo_track : !(muted & (UINT64_C(1) << track));
}

static void __not_in_flash_func(emit_event)(const uint8_t *e, uint32_t *on, uint32_t *off) {
    const uint8_t track = e[4], status = e[5], note = e[6], velocity = e[7];
    if (status == MIDI_CLOCK) { midi_send_clock(); return; }
    if (!enabled(track)) return;
    if (status < 0x80) {
        const uint8_t pair = status - 0x10;
        const uint32_t bit = 1u << pair;
        if (velocity) {
            cv_set_note(pair, note);
            gate_owner[pair] = track + 1;
            *on |= bit;
            active |= UINT64_C(1) << track;
        } else if (gate_owner[pair] == track + 1) {
            gate_owner[pair] = 0;
            *off |= bit;
            *on &= ~bit;
            active &= ~(UINT64_C(1) << track);
        }
        return;
    }
    const uint8_t kind = status & 0xf0;
    if (kind == 0x90 || kind == 0x80) {
        const uint16_t bit = 1u << (status & 15);
        const bool down = kind == 0x90 && velocity;
        const bool was_held = (held[track][note] & bit) != 0;
        if (down && !was_held) {
            held[track][note] |= bit;
            held_count[track]++;
            active |= UINT64_C(1) << track;
        } else if (!down && was_held) {
            held[track][note] &= ~bit;
            if (!--held_count[track]) active &= ~(UINT64_C(1) << track);
        }
    }
    uint8_t length = (status & 0xe0) == 0xc0 ? 2 : 3;
    midi_send_bytes(e + 5, length);
}

bool __not_in_flash_func(midi_song_dispatch)(uint32_t now, uint32_t *next) {
    // The first alarm may wake just before start; signed subtraction is safe
    // across timer wrap because a validated song is shorter than INT32_MAX us.
    const int32_t elapsed = (int32_t)(now - start_us);
    if (elapsed < 0) { *next = start_us; return true; }
    position = (uint32_t)elapsed < duration ? (uint32_t)elapsed : duration;

    uint32_t on = 0, off = 0, batch = 0;
    while (next_index < count && batch < MAX_IRQ_EVENTS) {
        const uint8_t *e = stream + next_index * EVENT_BYTES;
        uint32_t at = read_u32(e);
        if (at > position) break;
        if (on || off) {
            const uint8_t *prior = e - EVENT_BYTES;
            if (at != read_u32(prior)) {
                gate_commit(on, off);
                on = off = 0;
            }
        }
        emit_event(e, &on, &off);
        next_index++;
        batch++;
    }
    if (on || off) gate_commit(on, off);
    if (next_index == count && position == duration) return false;
    *next = start_us + (next_index < count ? read_u32(stream + next_index * EVENT_BYTES) : duration);
    return true;
}

void midi_song_toggle(uint8_t track, bool solo) {
    if (track >= tracks) return;
    if (solo) solo_track = solo_track == (int8_t)track ? -1 : (int8_t)track;
    else muted ^= UINT64_C(1) << track;
    if (!running) return;
    for (uint8_t tr = 0; tr < tracks; tr++) {
        if (enabled(tr)) continue;
        active &= ~(UINT64_C(1) << tr);
        held_count[tr] = 0;
        for (uint8_t note = 0; note < 128u; note++) {
            uint16_t channels = held[tr][note];
            held[tr][note] = 0;
            for (uint8_t ch = 0; channels; ch++, channels >>= 1) {
                if (channels & 1) midi_send_note_off(ch, note);
            }
        }
    }
    for (uint8_t pair = 0; pair < NUM_CV_CHANNELS; pair++) {
        if (gate_owner[pair] && !enabled(gate_owner[pair] - 1)) {
            gate_off(pair);
            gate_owner[pair] = 0;
        }
    }
}

void midi_song_view(uint32_t now, midi_song_view_t *view) {
    memset(view, 0, sizeof(*view));
    if (!stream) return;
    memcpy(view->title, stream - HEADER_BYTES + 20, sizeof(view->title));
    view->duration_us = duration;
    const int32_t elapsed = (int32_t)(now - start_us);
    view->position_us = running && elapsed > 0 ? (uint32_t)elapsed : position;
    if (view->position_us > duration) view->position_us = duration;
    view->tracks = tracks;
    view->present = present;
    view->active = active;
    view->muted = muted;
    view->solo = solo_track;
}

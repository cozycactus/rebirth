#ifndef REBIRTH_MIDI_SONG_H
#define REBIRTH_MIDI_SONG_H

#include <stdbool.h>
#include <stdint.h>

// This slot ends where the existing 128 KiB of clip saves begin.
#define MIDI_SONG_FLASH_OFFSET 0x000e0000u
#define MIDI_SONG_FLASH_BYTES  (1024u * 1024u)
#define MIDI_SONG_MAX_TRACKS   64u

typedef struct {
    char title[20];
    uint32_t duration_us;
    uint32_t position_us;
    uint64_t present;
    uint64_t active;
    uint64_t muted;
    int8_t solo;
    uint8_t tracks;
} midi_song_view_t;

// These functions are used under the playback lock, except mount/reset when
// the transport is stopped. The validated event stream is read in-place.
void midi_song_reset(void);
bool midi_song_mount(void);
bool midi_song_loaded(void);
void midi_song_begin(uint32_t now);
void midi_song_end(void);
// Process due events. The result is the next absolute 32-bit timer deadline.
bool midi_song_dispatch(uint32_t now, uint32_t *next);
void midi_song_toggle(uint8_t track, bool solo);
void midi_song_view(uint32_t now, midi_song_view_t *view);

bool transport_open_midi_song(void);
void transport_midi_song_toggle(uint8_t track, bool solo);
void transport_midi_song_view(midi_song_view_t *view);
bool ui_open_midi_song(void);

#endif

#include <string.h>
#include "sequencer.h"

// ─── Pattern access helpers ─────────────────────────────────────

pattern_t *seq_active_pattern(void) {
    return &g_seq.patterns[g_seq.active_pattern].variation[g_seq.active_variation];
}

pattern_t *seq_playing_pattern(void) {
    return &g_seq.patterns[g_seq.transport.current_pattern]
                .variation[g_seq.transport.current_variation];
}

// ─── Song editing ───────────────────────────────────────────────

bool song_append(uint8_t pattern_index, uint8_t variation, uint8_t repeats) {
    if (g_seq.song.length >= MAX_SONG_ENTRIES) return false;
    if (pattern_index >= MAX_PATTERNS) return false;
    if (variation > 1) return false;
    if (repeats == 0) return false;

    song_entry_t *e = &g_seq.song.entries[g_seq.song.length];
    e->pattern_index = pattern_index;
    e->variation = variation;
    e->repeats = repeats;
    g_seq.song.length++;
    return true;
}

bool song_insert(uint16_t position, uint8_t pattern_index, uint8_t variation, uint8_t repeats) {
    if (g_seq.song.length >= MAX_SONG_ENTRIES) return false;
    if (position > g_seq.song.length) return false;
    if (pattern_index >= MAX_PATTERNS) return false;
    if (variation > 1) return false;
    if (repeats == 0) return false;

    // Shift entries after position
    uint16_t count = g_seq.song.length - position;
    if (count > 0) {
        memmove(&g_seq.song.entries[position + 1],
                &g_seq.song.entries[position],
                count * sizeof(song_entry_t));
    }

    g_seq.song.entries[position].pattern_index = pattern_index;
    g_seq.song.entries[position].variation = variation;
    g_seq.song.entries[position].repeats = repeats;
    g_seq.song.length++;
    return true;
}

bool song_remove(uint16_t position) {
    if (position >= g_seq.song.length) return false;

    uint16_t count = g_seq.song.length - position - 1;
    if (count > 0) {
        memmove(&g_seq.song.entries[position],
                &g_seq.song.entries[position + 1],
                count * sizeof(song_entry_t));
    }
    g_seq.song.length--;
    return true;
}

bool song_move(uint16_t from, uint16_t to) {
    if (from >= g_seq.song.length || to >= g_seq.song.length) return false;
    if (from == to) return true;

    song_entry_t tmp = g_seq.song.entries[from];

    if (from < to) {
        memmove(&g_seq.song.entries[from],
                &g_seq.song.entries[from + 1],
                (to - from) * sizeof(song_entry_t));
    } else {
        memmove(&g_seq.song.entries[to + 1],
                &g_seq.song.entries[to],
                (from - to) * sizeof(song_entry_t));
    }
    g_seq.song.entries[to] = tmp;
    return true;
}

void song_clear(void) {
    g_seq.song.length = 0;
    g_seq.song.name[0] = '\0';
}

void song_set_end_mode(song_end_mode_t mode) {
    g_seq.song.end_mode = mode;
}

// ─── Pattern queue ──────────────────────────────────────────────

void transport_queue_pattern(uint8_t pattern_index, uint8_t variation) {
    if (pattern_index >= MAX_PATTERNS) return;
    if (variation > 1) return;
    transport_t *t = (transport_t *)&g_seq.transport;
    t->queued_pattern = pattern_index;
    t->queued_variation = variation;
}

void transport_clear_queue(void) {
    transport_t *t = (transport_t *)&g_seq.transport;
    t->queued_pattern = 0xFF;
    t->queued_variation = 0;
}

// ─── Track mutes ────────────────────────────────────────────────

void seq_set_track_mute(uint8_t track, bool muted) {
    if (track >= NUM_TRACKS) return;
    if (muted)
        g_seq.track_mutes |= (1u << track);
    else
        g_seq.track_mutes &= ~(1u << track);
}

void seq_toggle_track_mute(uint8_t track) {
    if (track >= NUM_TRACKS) return;
    g_seq.track_mutes ^= (1u << track);
}

uint8_t seq_get_track_mutes(void) {
    return g_seq.track_mutes;
}

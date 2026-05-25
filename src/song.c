#include <string.h>
#include "sequencer.h"

// ─── Pattern access helpers ─────────────────────────────────────

pattern_t *seq_active_pattern(uint8_t track) {
    if (track >= NUM_TRACKS) track = 0;
    return &g_seq.patterns[g_seq.active_pattern[track]];
}

pattern_t *seq_playing_pattern(uint8_t track) {
    if (track >= NUM_TRACKS) track = 0;
    return &g_seq.patterns[g_seq.transport.current_pattern[track]];
}

// ─── Song editing ───────────────────────────────────────────────

bool song_append(uint8_t pattern_index, uint8_t repeats) {
    if (pattern_index >= MAX_PATTERNS) return false;

    uint8_t patterns[NUM_TRACKS];
    for (uint8_t track = 0; track < NUM_TRACKS; track++) {
        patterns[track] = pattern_index;
    }
    return song_append_patterns(patterns, repeats);
}

bool song_append_patterns(const uint8_t pattern_index[NUM_TRACKS], uint8_t repeats) {
    if (!pattern_index) return false;
    if (repeats == 0) return false;
    for (uint8_t track = 0; track < NUM_TRACKS; track++) {
        if (pattern_index[track] >= MAX_PATTERNS) return false;
    }

    uint32_t lock_state = seq_lock();
    if (g_seq.song.length >= MAX_SONG_ENTRIES) {
        seq_unlock(lock_state);
        return false;
    }

    song_entry_t *e = &g_seq.song.entries[g_seq.song.length];
    memcpy(e->pattern_index, pattern_index, sizeof(e->pattern_index));
    e->repeats = repeats;
    g_seq.song.length++;
    seq_unlock(lock_state);
    return true;
}

bool song_insert(uint16_t position, uint8_t pattern_index, uint8_t repeats) {
    if (pattern_index >= MAX_PATTERNS) return false;

    uint8_t patterns[NUM_TRACKS];
    for (uint8_t track = 0; track < NUM_TRACKS; track++) {
        patterns[track] = pattern_index;
    }
    return song_insert_patterns(position, patterns, repeats);
}

bool song_insert_patterns(uint16_t position, const uint8_t pattern_index[NUM_TRACKS],
                          uint8_t repeats) {
    if (!pattern_index) return false;
    if (repeats == 0) return false;
    for (uint8_t track = 0; track < NUM_TRACKS; track++) {
        if (pattern_index[track] >= MAX_PATTERNS) return false;
    }

    uint32_t lock_state = seq_lock();
    if (g_seq.song.length >= MAX_SONG_ENTRIES || position > g_seq.song.length) {
        seq_unlock(lock_state);
        return false;
    }

    // Shift entries after position
    uint16_t count = g_seq.song.length - position;
    if (count > 0) {
        memmove(&g_seq.song.entries[position + 1],
                &g_seq.song.entries[position],
                count * sizeof(song_entry_t));
    }

    memcpy(g_seq.song.entries[position].pattern_index,
           pattern_index,
           sizeof(g_seq.song.entries[position].pattern_index));
    g_seq.song.entries[position].repeats = repeats;
    g_seq.song.length++;
    seq_unlock(lock_state);
    return true;
}

bool song_remove(uint16_t position) {
    uint32_t lock_state = seq_lock();
    if (position >= g_seq.song.length) {
        seq_unlock(lock_state);
        return false;
    }

    uint16_t count = g_seq.song.length - position - 1;
    if (count > 0) {
        memmove(&g_seq.song.entries[position],
                &g_seq.song.entries[position + 1],
                count * sizeof(song_entry_t));
    }
    g_seq.song.length--;
    seq_unlock(lock_state);
    return true;
}

bool song_move(uint16_t from, uint16_t to) {
    uint32_t lock_state = seq_lock();
    if (from >= g_seq.song.length || to >= g_seq.song.length) {
        seq_unlock(lock_state);
        return false;
    }
    if (from == to) {
        seq_unlock(lock_state);
        return true;
    }

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
    seq_unlock(lock_state);
    return true;
}

void song_clear(void) {
    uint32_t lock_state = seq_lock();
    g_seq.song.length = 0;
    g_seq.song.name[0] = '\0';
    seq_unlock(lock_state);
}

void song_set_end_mode(song_end_mode_t mode) {
    uint32_t lock_state = seq_lock();
    g_seq.song.end_mode = mode;
    seq_unlock(lock_state);
}

// ─── Pattern queue ──────────────────────────────────────────────

void transport_queue_pattern(uint8_t pattern_index) {
    if (pattern_index >= MAX_PATTERNS) return;
    for (uint8_t track = 0; track < NUM_TRACKS; track++) {
        transport_queue_track_pattern(track, pattern_index);
    }
}

void transport_queue_track_pattern(uint8_t track, uint8_t pattern_index) {
    if (track >= NUM_TRACKS) return;
    if (pattern_index >= MAX_PATTERNS) return;

    uint32_t lock_state = seq_lock();
    transport_t *t = (transport_t *)&g_seq.transport;
    t->queued_pattern[track] = NO_QUEUED_PATTERN;
    seq_unlock(lock_state);

#ifndef REBIRTH_HOST_TEST
    sequencer_prepare_queued_pattern(track, pattern_index);
#endif

    lock_state = seq_lock();
    t = (transport_t *)&g_seq.transport;
    t->queued_pattern[track] = pattern_index;
    seq_unlock(lock_state);
}

void transport_clear_queue(void) {
    for (uint8_t track = 0; track < NUM_TRACKS; track++) {
        transport_clear_track_queue(track);
    }
}

void transport_clear_track_queue(uint8_t track) {
    if (track >= NUM_TRACKS) return;
    uint32_t lock_state = seq_lock();
    transport_t *t = (transport_t *)&g_seq.transport;
    t->queued_pattern[track] = NO_QUEUED_PATTERN;
    seq_unlock(lock_state);
}

// ─── Track mutes ────────────────────────────────────────────────

void seq_set_track_mute(uint8_t track, bool muted) {
    if (track >= NUM_TRACKS) return;
    uint32_t lock_state = seq_lock();
    if (muted)
        g_seq.track_mutes |= (1u << track);
    else
        g_seq.track_mutes &= ~(1u << track);
    seq_unlock(lock_state);
}

void seq_toggle_track_mute(uint8_t track) {
    if (track >= NUM_TRACKS) return;
    uint32_t lock_state = seq_lock();
    g_seq.track_mutes ^= (1u << track);
    seq_unlock(lock_state);
}

uint8_t seq_get_track_mutes(void) {
    uint32_t lock_state = seq_lock();
    uint8_t mutes = g_seq.track_mutes;
    seq_unlock(lock_state);
    return mutes;
}

#include <string.h>
#include "pico/stdlib.h"
#include "sequencer.h"
#include "kit.h"

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

// A list edit shifts every row after the edit point with Core 0's interrupts
// off (seq_lock), and Core 1's timer waits on the lock every pulse. The rows
// are whole words, so each is copied as 17 word loads and stores, unrolled in
// the source so that every build (Debug at -Og too) copies a row the same way,
// from RAM and never inlined into a caller in flash; the C library's memmove
// copies these rows a byte at a time. GCC does not turn the unrolled words
// into a memmove call; CMakeLists.txt keeps it from vectorising this file,
// which would triple the copy's size in RAM. A row's source and destination
// never overlap (a shift is at least one row), so only the order of the rows
// depends on the overlap. Nothing else runs from flash under the lock: a new
// row is built before it is taken and stored with this copy.
_Static_assert(SONG_ROW_WORDS == 17, "song_move_rows() copies 17 words a row");
#define COPY_4_WORDS(d, s, i) do {                                                  \
        uint32_t w0 = (s)[(i)], w1 = (s)[(i) + 1], w2 = (s)[(i) + 2], w3 = (s)[(i) + 3]; \
        (d)[(i)] = w0; (d)[(i) + 1] = w1; (d)[(i) + 2] = w2; (d)[(i) + 3] = w3;            \
    } while (0)
#define COPY_ROW(dst, src) do {                                                     \
        uint32_t *d_ = (dst)->words;                                                \
        const uint32_t *s_ = (src)->words;                                          \
        COPY_4_WORDS(d_, s_, 0);                                                    \
        COPY_4_WORDS(d_, s_, 4);                                                    \
        COPY_4_WORDS(d_, s_, 8);                                                    \
        COPY_4_WORDS(d_, s_, 12);                                                   \
        d_[16] = s_[16];                                                            \
    } while (0)

// One copy in RAM: GCC would otherwise add a second, specialised for one row.
#if defined(__GNUC__) && !defined(__clang__)
__attribute__((noclone))
#endif
void __no_inline_not_in_flash_func(song_move_rows)(song_entry_t *dst, const song_entry_t *src,
                                                   uint16_t rows) {
    if (!rows || dst == src) return;
    // As integers: a single row may come from another array (a row built on
    // the stack), where comparing the pointers themselves is undefined.
    if ((uintptr_t)dst < (uintptr_t)src) {
        for (uint16_t r = 0; r < rows; r++) COPY_ROW(&dst[r], &src[r]);
    } else {
        for (uint16_t r = rows; r--;) COPY_ROW(&dst[r], &src[r]);
    }
}

// A whole new row, padding 0, built before the lock is taken.
static void make_row(song_entry_t *row, const uint8_t pattern_index[NUM_TRACKS], uint8_t repeats) {
    memset(row, 0, sizeof(*row));
    memcpy(row->pattern_index, pattern_index, sizeof(row->pattern_index));
    row->repeats = repeats;
}

// While Song mode plays, list edits keep the playing entry where it is, so the
// entry that follows it in the edited list plays next. Caller holds seq_lock.
static transport_t *playing_song_transport(void) {
    transport_t *t = (transport_t *)&g_seq.transport;
    if (t->mode != PLAY_MODE_SONG || t->state != TRANSPORT_PLAYING) return NULL;
    return t;
}

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

    song_entry_t row;
    make_row(&row, pattern_index, repeats);
    uint32_t lock_state = seq_lock();
    if (g_seq.song.length >= MAX_SONG_ENTRIES) {
        seq_unlock(lock_state);
        return false;
    }

    song_move_rows(&g_seq.song.entries[g_seq.song.length], &row, 1);
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

    song_entry_t row;
    make_row(&row, pattern_index, repeats);
    uint32_t lock_state = seq_lock();
    if (g_seq.song.length >= MAX_SONG_ENTRIES || position > g_seq.song.length) {
        seq_unlock(lock_state);
        return false;
    }

    // Shift entries after position
    uint16_t count = g_seq.song.length - position;
    if (count > 0) {
        song_move_rows(&g_seq.song.entries[position + 1], &g_seq.song.entries[position], count);
    }

    song_move_rows(&g_seq.song.entries[position], &row, 1);
    g_seq.song.length++;
    transport_t *t = playing_song_transport();
    if (t && position <= t->song_position) t->song_position++;
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
        song_move_rows(&g_seq.song.entries[position], &g_seq.song.entries[position + 1], count);
    }
    g_seq.song.length--;
    uint16_t vacated = g_seq.song.length;
    transport_t *t = playing_song_transport();
    if (t && position < t->song_position) {
        t->song_position--;
    } else if (t && position == t->song_position) {
        // The removed entry finishes its pass, then the entry after it plays.
        // Past the end means "restart at entry 0" to the song advance.
        t->song_repeat_count = 1;
        t->song_position = (position > 0) ? (uint16_t)(position - 1) : g_seq.song.length;
    }
    seq_unlock(lock_state);
    // Rows past the length are Core 0's alone (Core 1 reads only the song's
    // rows), and they stay zero, as a save writes them.
    memset(&g_seq.song.entries[vacated], 0, sizeof(g_seq.song.entries[vacated]));
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

    song_entry_t tmp;
    song_move_rows(&tmp, &g_seq.song.entries[from], 1);

    if (from < to) {
        song_move_rows(&g_seq.song.entries[from], &g_seq.song.entries[from + 1],
                       (uint16_t)(to - from));
    } else {
        song_move_rows(&g_seq.song.entries[to + 1], &g_seq.song.entries[to],
                       (uint16_t)(from - to));
    }
    song_move_rows(&g_seq.song.entries[to], &tmp, 1);
    transport_t *t = playing_song_transport();
    if (t && t->song_position < g_seq.song.length) {
        uint16_t pos = t->song_position;
        if (from == pos) {
            pos = to;
        } else if (from < pos && to >= pos) {
            pos--;
        } else if (from > pos && to <= pos) {
            pos++;
        }
        t->song_position = pos;
    }
    seq_unlock(lock_state);
    return true;
}

void song_clear(void) {
    uint32_t lock_state = seq_lock();
    g_seq.song.length = 0;
    g_seq.song.name[0] = '\0';
    seq_unlock(lock_state);
    // Outside the lock, as in song_remove(): no row is the song's now.
    memset(g_seq.song.entries, 0, sizeof(g_seq.song.entries));
}

void song_set_end_mode(song_end_mode_t mode) {
    uint32_t lock_state = seq_lock();
    g_seq.song.end_mode = mode;
    seq_unlock(lock_state);
}

// ─── Track mutes ────────────────────────────────────────────────

void seq_set_track_mute(uint8_t track, bool muted) {
    if (track >= NUM_TRACKS) return;
    uint32_t lock_state = seq_lock();
    if (muted)
        g_seq.track_mutes |= TRACK_BIT(track);
    else
        g_seq.track_mutes &= ~TRACK_BIT(track);
    seq_unlock(lock_state);
}

// Called with no lock held: each refresh takes seq_lock itself.
static void refresh_track_rows(uint8_t track) {
    for (uint8_t p = 0; p < MAX_PATTERNS; p++) {
        sequencer_refresh_track_playback(track, p);
    }
}

track_mask_t seq_kit_tracks(void) {
    track_mask_t mask = 0;
    // One location per lock section, as below.
    for (uint8_t p = 0; p < MAX_PATTERNS; p++) {
        uint32_t lock_state = seq_lock();
        mask |= kit_track_mask(&g_seq.patterns[p], 1);
        seq_unlock(lock_state);
    }
    return mask;
}

bool seq_set_track_config(uint8_t track, const track_config_t *config) {
    if (track >= NUM_TRACKS || !config) return false;
    if (config->type == TRACK_TYPE_KIT) {
        if (track >= KIT_TRACK_SLOTS) return false;
        // At most MAX_KIT_TRACKS kits play at once, in Song mode and mixed launches too.
        track_mask_t kits = seq_kit_tracks();
        if (!(kits & TRACK_BIT(track))) {
            uint8_t count = 0;
            for (; kits; kits &= (kits - 1u)) count++;
            if (count >= MAX_KIT_TRACKS) return false;
        }
    }
    // One location per lock section keeps Core 0's interrupts-off windows short;
    // Core 1 reads routing only from the prepared rows refreshed below.
    for (uint8_t p = 0; p < MAX_PATTERNS; p++) {
        uint32_t lock_state = seq_lock();
        // Routing is the track's; the stored mute stays per location.
        bool muted = g_seq.patterns[p].tracks[track].muted;
        g_seq.patterns[p].tracks[track] = *config;
        g_seq.patterns[p].tracks[track].muted = muted;
        seq_unlock(lock_state);
    }
    refresh_track_rows(track);
    return true;
}

void seq_set_kit_config(uint8_t track, const kit_config_t *config) {
    if (track >= NUM_TRACKS || !config) return;
    kit_config_t clamped = *config;
    kit_config_clamp(&clamped);
    uint32_t lock_state = seq_lock();
    g_seq.kit_config[track] = clamped;
    seq_unlock(lock_state);
    refresh_track_rows(track);
}

void seq_clear_stored_mutes(track_mask_t mask) {
    track_mask_t cleared = 0;
    // Stored mutes reach Core 1 only through the prepared rows refreshed below,
    // so they are cleared one location per lock section.
    for (uint8_t p = 0; p < MAX_PATTERNS; p++) {
        uint32_t lock_state = seq_lock();
        for (uint8_t track = 0; track < NUM_TRACKS; track++) {
            if (!(mask & TRACK_BIT(track)) || !g_seq.patterns[p].tracks[track].muted) continue;
            g_seq.patterns[p].tracks[track].muted = false;
            cleared |= TRACK_BIT(track);
        }
        seq_unlock(lock_state);
    }
    for (uint8_t track = 0; track < NUM_TRACKS; track++) {
        if (cleared & TRACK_BIT(track)) refresh_track_rows(track);
    }
}

void seq_toggle_track_mute(uint8_t track) {
    if (track >= NUM_TRACKS) return;
    track_mask_t bit = TRACK_BIT(track);
    uint32_t lock_state = seq_lock();
    bool unmute = (seq_track_mutes_locked() & bit) != 0;
    if (unmute) {
        g_seq.track_mutes &= ~bit;
    } else {
        g_seq.track_mutes |= bit;
    }
    seq_unlock(lock_state);
    if (unmute) seq_clear_stored_mutes(bit);
}

track_mask_t seq_track_mutes_locked(void) {
    track_mask_t mutes = g_seq.track_mutes;
    for (uint8_t track = 0; track < NUM_TRACKS; track++) {
        uint8_t playing = g_seq.transport.current_pattern[track];
        if (playing < MAX_PATTERNS && g_seq.patterns[playing].tracks[track].muted) {
            mutes |= TRACK_BIT(track);
        }
    }
    return mutes;
}

track_mask_t seq_get_track_mutes(void) {
    uint32_t lock_state = seq_lock();
    track_mask_t mutes = seq_track_mutes_locked();
    seq_unlock(lock_state);
    return mutes;
}

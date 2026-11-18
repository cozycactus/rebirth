#ifndef MOCK_HARDWARE
#include "pico/stdlib.h"
#include "hardware/flash.h"
#include "hardware/sync.h"
#include "pico/multicore.h"
#endif

#include <string.h>
#include "sequencer.h"
#include "flash_storage.h"
#include "panel_input.h"
#include "mcp23017.h"
#include "encoder.h"
#include "kit.h"
#include <stddef.h>

// ─── Flash layout ──────────────────────────────────────────────
//
// v7 lives immediately before the MIDI Song slot. A new save leaves the
// last 128 KiB intact so the original eight-track firmware can still read it.
#define FLASH_PAGE      256u
#define STORE_OFFSET    0x000b0000u
#define STORE_RESERVED  0x00030000u
#define STORE_ERASE     STORE_RESERVED
#define OLD_STORE       0x001e0000u
#define OLD_TRACKS      8

#define PAGE_ROUND(n)   (((n) + FLASH_PAGE - 1u) & ~(FLASH_PAGE - 1u))

#define HDR_OFFSET      0u
#define HDR_SIZE        FLASH_PAGE

#define PAT_OFFSET      HDR_SIZE
#define PAT_BYTES       (sizeof(pattern_t) * MAX_PATTERNS)
#define PAT_SIZE        PAGE_ROUND(PAT_BYTES)

#define SONG_OFFSET     (PAT_OFFSET + PAT_SIZE)
#define SONG_BYTES      sizeof(song_t)
#define SONG_SIZE       PAGE_ROUND(SONG_BYTES)

#define KIT_OFFSET      (SONG_OFFSET + SONG_SIZE)
#define KIT_BYTES       (sizeof(kit_config_t) * NUM_TRACKS)
#define KIT_SIZE        PAGE_ROUND(KIT_BYTES)

// On-flash records from the eight-track firmware, including its ABI padding.
typedef struct {
    step_t steps[OLD_TRACKS][STEPS_PER_PATTERN];
    track_config_t tracks[OLD_TRACKS];
    uint8_t length, track_length[OLD_TRACKS], swing;
    char name[9];
} pattern_v5_t;
typedef struct {
    step_t steps[OLD_TRACKS][STEPS_PER_PATTERN];
    track_config_t tracks[OLD_TRACKS];
    uint8_t length, track_length[OLD_TRACKS], swing;
    char name[9];
    uint32_t kit_steps[OLD_TRACKS][STEPS_PER_PATTERN];
} pattern_v6_t;
typedef struct {
    struct { uint8_t pattern_index[OLD_TRACKS], repeats; } entries[MAX_SONG_ENTRIES];
    uint16_t length;
    song_end_mode_t end_mode;
    char name[9];
} old_song_t;

#if defined(__arm__)
_Static_assert(sizeof(pattern_v5_t) == 459 && sizeof(pattern_v6_t) == 972, "legacy pattern ABI");
_Static_assert(sizeof(old_song_t) == 2316, "legacy song ABI");
#endif
#define V5_PAT_BYTES    (sizeof(pattern_v5_t) * MAX_PATTERNS)
#define V5_SONG_OFFSET  (PAT_OFFSET + PAGE_ROUND(V5_PAT_BYTES))
#define V6_PAT_BYTES    (sizeof(pattern_v6_t) * MAX_PATTERNS)
#define V6_SONG_OFFSET  (PAT_OFFSET + PAGE_ROUND(V6_PAT_BYTES))
#define V6_KIT_OFFSET   (V6_SONG_OFFSET + PAGE_ROUND(sizeof(old_song_t)))
_Static_assert(HDR_SIZE + PAT_SIZE + SONG_SIZE + KIT_SIZE <= STORE_RESERVED,
               "64-track save exceeds its flash slot");

// ─── Header ────────────────────────────────────────────────────

#define MAGIC   0x52425448u     // 'RBTH'
#define VERSION 7u
#define V5_VERSION 5u
#define LEGACY_VERSION 4u

typedef struct {
    uint32_t magic;
    uint8_t  version;
    uint8_t  _reserved[3];
    uint32_t checksum;
    uint8_t  _pad[FLASH_PAGE - 12u];
} store_header_t;

_Static_assert(sizeof(store_header_t) == FLASH_PAGE,
               "store_header_t must be exactly one flash page");

// ─── API ───────────────────────────────────────────────────────

#ifndef MOCK_HARDWARE
// Program complete pages directly from the stopped sequencer's RAM. Only a
// partial final page needs padding. The caller has parked Core 1.
static void program_payload(uint32_t offset, const void *data, size_t bytes) {
    const uint8_t *src = data;
    size_t aligned = bytes - bytes % FLASH_PAGE;
    if (aligned) flash_range_program(offset, src, aligned);
    if (aligned < bytes) {
        uint8_t tail[FLASH_PAGE];
        memset(tail, 0xff, sizeof(tail));
        memcpy(tail, src + aligned, bytes - aligned);
        flash_range_program(offset + aligned, tail, sizeof(tail));
    }
}

// Panel keys, step buttons and encoder buttons held or changed during the
// stall must not fire afterwards.
static void resync_inputs(void) {
    panel_input_resync();
    mcp23017_resync();
    encoder_resync();
}

static uint32_t crc32_update(uint32_t crc, const void *data, size_t length) {
    const uint8_t *bytes = data;
    for (size_t i = 0; i < length; i++) {
        crc ^= bytes[i];
        for (unsigned bit = 0; bit < 8; bit++) {
            crc = (crc >> 1) ^ ((crc & 1u) ? 0xEDB88320u : 0u);
        }
    }
    return crc;
}

// Version 7: patterns + song + kit settings.
static uint32_t payload_checksum(const void *patterns, const void *song, const void *kits) {
    uint32_t crc = crc32_update(UINT32_MAX, patterns, PAT_BYTES);
    crc = crc32_update(crc, song, SONG_BYTES);
    return ~crc32_update(crc, kits, KIT_BYTES);
}

// Version 5: legacy patterns + song.
static uint32_t payload_checksum_v5(const void *patterns, const void *song) {
    return ~crc32_update(crc32_update(UINT32_MAX, patterns, V5_PAT_BYTES), song, sizeof(old_song_t));
}

static bool song_valid(const song_t *song) {
    if (song->length > MAX_SONG_ENTRIES || song->name[8] != '\0') return false;
    if (song->end_mode != SONG_END_LOOP && song->end_mode != SONG_END_STOP) return false;
    for (uint16_t entry = 0; entry < song->length; entry++) {
        for (uint8_t track = 0; track < NUM_TRACKS; track++) {
            if (song->entries[entry].pattern_index[track] >= MAX_PATTERNS) return false;
        }
    }
    return true;
}

static bool pattern_core_valid(const step_t steps[][STEPS_PER_PATTERN], const track_config_t *tracks,
                               const uint8_t *lengths, uint8_t count, uint8_t length,
                               uint8_t swing, const char name[9], bool allow_kit) {
    if (!length || length > STEPS_PER_PATTERN || swing > 99 || name[8]) return false;
    for (uint8_t track = 0; track < count; track++) {
        const track_config_t *tc = &tracks[track];
        uint8_t mute;
        memcpy(&mute, &tc->muted, sizeof(mute));
        bool type_ok = tc->type == TRACK_TYPE_SYNTH || tc->type == TRACK_TYPE_DRUM ||
                       (allow_kit && track < KIT_TRACK_SLOTS && tc->type == TRACK_TYPE_KIT);
        if (mute > 1 || lengths[track] > STEPS_PER_PATTERN || !type_ok ||
            tc->midi_channel > 15 || tc->drum_note > 127 ||
            tc->output_mode < OUTPUT_MIDI_ONLY || tc->output_mode > OUTPUT_BOTH ||
            tc->cv_channel >= NUM_CV_CHANNELS ||
            tc->gate_length_pct < 10 || tc->gate_length_pct > 100) return false;
        for (uint8_t step = 0; step < STEPS_PER_PATTERN; step++) {
            const step_t *s = &steps[track][step];
            if (s->note > 127 || s->velocity > 127 || (s->flags & 0xF0u)) return false;
        }
    }
    return true;
}

static bool payload_valid(const pattern_t *patterns, const song_t *song,
                          const kit_config_t *kits) {
    if (!song_valid(song)) return false;
    for (uint8_t pattern = 0; pattern < MAX_PATTERNS; pattern++) {
        const pattern_t *p = &patterns[pattern];
        if (!pattern_core_valid(p->steps, p->tracks, p->track_length, NUM_TRACKS,
                                p->length, p->swing, p->name, true)) return false;
        for (uint8_t track = 0; track < KIT_TRACK_SLOTS; track++) {
            for (uint8_t step = 0; step < STEPS_PER_PATTERN; step++) {
                uint32_t word = p->kit_steps[track][step];
                if ((word >> 24) != 0 || (word & 3u) > 1u) return false;
            }
        }
    }
    if (__builtin_popcountll(kit_track_mask(patterns, MAX_PATTERNS)) > MAX_KIT_TRACKS) return false;
    for (uint8_t track = 0; track < NUM_TRACKS; track++) {
        if (!kit_config_valid(&kits[track])) return false;
    }
    return true;
}

static bool old_image_valid(const uint8_t *base) {
    const store_header_t *h = (const store_header_t *)base;
    if (h->magic != MAGIC || (h->version != 4 && h->version != 5 && h->version != 6)) return false;
    const bool kit = h->version == 6;
    const old_song_t *song = (const old_song_t *)(base + (kit ? V6_SONG_OFFSET : V5_SONG_OFFSET));
    if (song->length > MAX_SONG_ENTRIES || song->name[8] ||
        (song->end_mode != SONG_END_LOOP && song->end_mode != SONG_END_STOP)) return false;
    for (unsigned i = 0; i < song->length; i++)
        for (unsigned t = 0; t < OLD_TRACKS; t++)
            if (song->entries[i].pattern_index[t] >= MAX_PATTERNS) return false;
    uint8_t kits = 0;
    const size_t stride = kit ? sizeof(pattern_v6_t) : sizeof(pattern_v5_t);
    for (unsigned i = 0; i < MAX_PATTERNS; i++) {
        pattern_v5_t p; // v5 stride is odd with the RP2040 ABI
        const uint8_t *record = base + PAT_OFFSET + stride * i;
        memcpy(&p, record, sizeof(p));
        if (!pattern_core_valid(p.steps, p.tracks, p.track_length, OLD_TRACKS,
                                p.length, p.swing, p.name, kit)) return false;
        if (kit) {
            const pattern_v6_t *v6 = (const pattern_v6_t *)record;
            for (unsigned t = 0; t < OLD_TRACKS; t++) {
                if (p.tracks[t].type == TRACK_TYPE_KIT) kits |= 1u << t;
                for (unsigned j = 0; j < STEPS_PER_PATTERN; j++) {
                    uint32_t word = v6->kit_steps[t][j];
                    if (word >> 24 || (word & 3u) > 1u) return false;
                }
            }
        }
    }
    if (!kit) return h->version == 4 || h->checksum == payload_checksum_v5(base + PAT_OFFSET, song);
    const kit_config_t *settings = (const kit_config_t *)(base + V6_KIT_OFFSET);
    if (__builtin_popcount(kits) > MAX_KIT_TRACKS) return false;
    for (unsigned t = 0; t < OLD_TRACKS; t++) if (!kit_config_valid(&settings[t])) return false;
    uint32_t crc = crc32_update(UINT32_MAX, base + PAT_OFFSET, V6_PAT_BYTES);
    crc = crc32_update(crc, song, sizeof(*song));
    return h->checksum == ~crc32_update(crc, settings, OLD_TRACKS * sizeof(*settings));
}
#endif

bool flash_storage_is_valid(void) {
#ifndef MOCK_HARDWARE
    const uint8_t *base = (const uint8_t *)(XIP_BASE + STORE_OFFSET);
    const store_header_t *h = (const store_header_t *)base;
    if (h->magic != MAGIC) return old_image_valid((const uint8_t *)(XIP_BASE + OLD_STORE));
    if (h->version == VERSION) {
        const pattern_t *patterns = (const pattern_t *)(base + PAT_OFFSET);
        const song_t *song = (const song_t *)(base + SONG_OFFSET);
        const kit_config_t *kits = (const kit_config_t *)(base + KIT_OFFSET);
        return payload_valid(patterns, song, kits) &&
               h->checksum == payload_checksum(patterns, song, kits);
    }
    return false;
#else
    return false;
#endif
}

bool flash_storage_save(void) {
    // Storage and transport commands are issued by Core 0. Core 1 can stop
    // playback, but cannot start it while this synchronous operation runs.
    if (g_seq.transport.state != TRANSPORT_STOPPED) return false;
#ifndef MOCK_HARDWARE
    if (!payload_valid(g_seq.patterns, &g_seq.song, g_seq.kit_config)) return false;
    // Build the 256-byte header page
    static store_header_t hdr;
    memset(&hdr, 0xFF, sizeof(hdr));    // 0xFF = erased flash default
    hdr.magic   = MAGIC;
    hdr.version = (uint8_t)VERSION;

    // Core 0 owns edits and Save is synchronous. Stopped Core 1 never
    // writes patterns, the song or kit settings.
    hdr.checksum = payload_checksum(g_seq.patterns, &g_seq.song, g_seq.kit_config);

    // Pause Core 1 so it won't execute from flash during the write
    // Finish any Stop/Note Off bytes before UART interrupts are suspended.
    midi_wait_output_idle();
    multicore_lockout_start_blocking();
    // Until Core 1 was parked, its timer ISR could still queue an overflow
    // panic. Send that too while the UART interrupt can still run.
    midi_wait_output_idle();
    uint32_t ints = save_and_disable_interrupts();

    flash_range_erase(STORE_OFFSET, STORE_ERASE);

    program_payload(STORE_OFFSET + PAT_OFFSET, g_seq.patterns, PAT_BYTES);
    program_payload(STORE_OFFSET + SONG_OFFSET, &g_seq.song, SONG_BYTES);
    program_payload(STORE_OFFSET + KIT_OFFSET, g_seq.kit_config, KIT_BYTES);

    // Mark this save valid only after all payloads have been programmed.
    flash_range_program(STORE_OFFSET + HDR_OFFSET,
                        (const uint8_t *)&hdr, HDR_SIZE);

    restore_interrupts(ints);
    multicore_lockout_end_blocking();
    // Whether or not the new save validates.
    resync_inputs();
    return ((const store_header_t *)(XIP_BASE + STORE_OFFSET))->magic == MAGIC && flash_storage_is_valid();
#else
    return false;
#endif
}

bool flash_storage_load(void) {
    if (g_seq.transport.state != TRANSPORT_STOPPED) return false;
#ifndef MOCK_HARDWARE
    if (!flash_storage_is_valid()) return false;

    const uint8_t *base = (const uint8_t *)(XIP_BASE + STORE_OFFSET);
    const store_header_t *h = (const store_header_t *)base;
    bool current = h->magic == MAGIC;
    const uint8_t *old = (const uint8_t *)(XIP_BASE + OLD_STORE);
    bool v6 = !current && ((const store_header_t *)old)->version == 6;
    midi_wait_output_idle();
    for (uint8_t i = 0; i < MAX_PATTERNS; i++) {
        pattern_t *dst = &g_seq.patterns[i];
        uint32_t lock = seq_lock();
        if (current) {
            memcpy(dst, base + PAT_OFFSET + i * sizeof(*dst), sizeof(*dst));
        } else {
            const uint8_t *src = old + PAT_OFFSET + i * (v6 ? sizeof(pattern_v6_t) : sizeof(pattern_v5_t));
            pattern_v5_t p;
            memcpy(&p, src, sizeof(p));
            pattern_init(dst, i);
            memcpy(dst->steps, p.steps, sizeof(p.steps));
            memcpy(dst->tracks, p.tracks, sizeof(p.tracks));
            memcpy(dst->track_length, p.track_length, sizeof(p.track_length));
            memcpy(dst->name, p.name, sizeof(p.name));
            dst->length = p.length;
            dst->swing = p.swing;
            if (v6) memcpy(dst->kit_steps, ((const pattern_v6_t *)src)->kit_steps, sizeof(dst->kit_steps));
        }
        seq_unlock(lock);
    }
    if (current) {
        memcpy(&g_seq.song, base + SONG_OFFSET, SONG_BYTES);
        memcpy(g_seq.kit_config, base + KIT_OFFSET, KIT_BYTES);
    } else {
        const old_song_t *song = (const old_song_t *)(old + (v6 ? V6_SONG_OFFSET : V5_SONG_OFFSET));
        memset(&g_seq.song, 0, sizeof(g_seq.song));
        g_seq.song.length = song->length;
        g_seq.song.end_mode = song->end_mode;
        memcpy(g_seq.song.name, song->name, sizeof(song->name));
        for (unsigned i = 0; i < song->length; i++) {
            memcpy(g_seq.song.entries[i].pattern_index, song->entries[i].pattern_index, OLD_TRACKS);
            g_seq.song.entries[i].repeats = song->entries[i].repeats;
        }
        for (unsigned t = 0; t < NUM_TRACKS; t++) kit_config_defaults(&g_seq.kit_config[t]);
        if (v6) memcpy(g_seq.kit_config, old + V6_KIT_OFFSET, OLD_TRACKS * sizeof(kit_config_t));
    }
    for (uint8_t pattern = 0; pattern < MAX_PATTERNS; pattern++) {
        for (uint8_t track = 0; track < NUM_TRACKS; track++) {
            sequencer_refresh_track_playback(track, pattern);
        }
    }
    resync_inputs();
    return true;
#else
    return false;
#endif
}

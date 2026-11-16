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
// Last 128 KB of the 2 MB Pico flash is reserved for storage.
//
// Version 6 (written):
//   Byte 0              : Header (1 page = 256 bytes)
//   Byte 256            : Patterns (4 banks x 8 slots x sizeof(pattern_t))
//   Byte 256+PAT        : Song (padded to page boundary)
//   Byte 256+PAT+SONG   : Kit settings, kit_config[NUM_TRACKS] (page-padded)
//
// Versions 4 and 5 (load only): the same header, then patterns as
// pattern_v5_t records, then the song at 256 + page-rounded legacy patterns.
// Version 4 has no checksum; version 5 checksums legacy patterns + song.
//
// RP2040 flash constants (always these values for the Pico):
#define FLASH_PAGE      256u
#define FLASH_SECTOR    4096u

#define FLASH_SZ        (2u * 1024u * 1024u)
#define STORE_RESERVED  (128u * 1024u)
#define STORE_OFFSET    (FLASH_SZ - STORE_RESERVED)   // raw flash offset
#define STORE_ERASE     STORE_RESERVED                 // erase the full 128 KB

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

// Versions 4 and 5: patterns were pattern_t before kit_steps was added.
// Frozen: this is the pre-v6 pattern_t, the record v4 and v5 saves were written
// with. Never change it to follow pattern_t; and step_t and track_config_t, which
// it embeds, must not change size or layout, or v4/v5 saves no longer load.
typedef struct {
    step_t         steps[NUM_TRACKS][STEPS_PER_PATTERN];
    track_config_t tracks[NUM_TRACKS];
    uint8_t        length;
    uint8_t        track_length[NUM_TRACKS];
    uint8_t        swing;
    char           name[9];
} pattern_v5_t;

// A legacy record is the start of today's pattern_t, field for field.
_Static_assert(offsetof(pattern_v5_t, steps) == offsetof(pattern_t, steps), "v5 steps");
_Static_assert(offsetof(pattern_v5_t, tracks) == offsetof(pattern_t, tracks), "v5 tracks");
_Static_assert(offsetof(pattern_v5_t, length) == offsetof(pattern_t, length), "v5 length");
_Static_assert(offsetof(pattern_v5_t, track_length) == offsetof(pattern_t, track_length),
               "v5 track_length");
_Static_assert(offsetof(pattern_v5_t, swing) == offsetof(pattern_t, swing), "v5 swing");
_Static_assert(offsetof(pattern_v5_t, name) == offsetof(pattern_t, name), "v5 name");
_Static_assert(sizeof(pattern_v5_t) <= offsetof(pattern_t, kit_steps),
               "a v5 record must end before kit_steps");
// The records as the RP2040 (short enums) writes them: saves made by earlier firmware
// must keep loading. A host build with -fshort-enums (CI) runs the same code.
#if defined(__arm__)
_Static_assert(sizeof(pattern_v5_t) == 459, "v4/v5 pattern record on the RP2040");
_Static_assert(offsetof(pattern_t, kit_steps) == 460, "v6 kit_steps offset on the RP2040");
_Static_assert(sizeof(pattern_t) == 972, "v6 pattern record on the RP2040");
#endif

#define V5_PAT_BYTES    (sizeof(pattern_v5_t) * MAX_PATTERNS)
#define V5_SONG_OFFSET  (PAT_OFFSET + PAGE_ROUND(V5_PAT_BYTES))

// Sanity: total storage fits in reserved region
_Static_assert(HDR_SIZE + PAT_SIZE + SONG_SIZE + KIT_SIZE <= STORE_RESERVED,
               "flash storage too large for reserved region");
_Static_assert(V5_SONG_OFFSET + PAGE_ROUND(sizeof(song_t)) <= STORE_RESERVED,
               "legacy flash storage too large for reserved region");

// ─── Header ────────────────────────────────────────────────────

#define MAGIC   0x52425448u     // 'RBTH'
#define VERSION 6u
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
// The save's copy of the patterns, padded to whole flash pages.
static uint8_t pattern_buf[PAT_SIZE];

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

// Version 6: patterns + song + kit settings.
static uint32_t payload_checksum(const void *patterns, const void *song, const void *kits) {
    uint32_t crc = crc32_update(UINT32_MAX, patterns, PAT_BYTES);
    crc = crc32_update(crc, song, SONG_BYTES);
    return ~crc32_update(crc, kits, KIT_BYTES);
}

// Version 5: legacy patterns + song.
static uint32_t payload_checksum_v5(const void *patterns, const void *song) {
    return ~crc32_update(crc32_update(UINT32_MAX, patterns, V5_PAT_BYTES), song, SONG_BYTES);
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

// The part of a pattern record that versions 4, 5 and 6 share.
static bool pattern_core_valid(const pattern_v5_t *p, bool allow_kit) {
    if (p->length < 1 || p->length > STEPS_PER_PATTERN ||
        p->swing > 99 || p->name[8] != '\0') return false;
    for (uint8_t track = 0; track < NUM_TRACKS; track++) {
        const track_config_t *tc = &p->tracks[track];
        uint8_t mute;
        memcpy(&mute, &tc->muted, sizeof(mute));
        bool type_ok = tc->type == TRACK_TYPE_SYNTH || tc->type == TRACK_TYPE_DRUM ||
                       (allow_kit && tc->type == TRACK_TYPE_KIT);
        if (mute > 1 || p->track_length[track] > STEPS_PER_PATTERN || !type_ok ||
            tc->midi_channel > 15 || tc->drum_note > 127 ||
            tc->output_mode < OUTPUT_MIDI_ONLY || tc->output_mode > OUTPUT_BOTH ||
            tc->cv_channel >= NUM_CV_CHANNELS ||
            tc->gate_length_pct < 10 || tc->gate_length_pct > 100) return false;
        for (uint8_t step = 0; step < STEPS_PER_PATTERN; step++) {
            const step_t *s = &p->steps[track][step];
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
        // pattern_t starts with a pattern_v5_t, field for field.
        if (!pattern_core_valid((const pattern_v5_t *)p, true)) return false;
        for (uint8_t track = 0; track < NUM_TRACKS; track++) {
            for (uint8_t step = 0; step < STEPS_PER_PATTERN; step++) {
                uint32_t word = p->kit_steps[track][step];
                if ((word >> 24) != 0 || (word & 3u) > 1u) return false;
            }
        }
    }
    if (__builtin_popcount(kit_track_mask(patterns, MAX_PATTERNS)) > MAX_KIT_TRACKS) return false;
    for (uint8_t track = 0; track < NUM_TRACKS; track++) {
        if (!kit_config_valid(&kits[track])) return false;
    }
    return true;
}

static bool payload_valid_v5(const uint8_t *base) {
    if (!song_valid((const song_t *)(base + V5_SONG_OFFSET))) return false;
    for (uint8_t pattern = 0; pattern < MAX_PATTERNS; pattern++) {
        pattern_v5_t record;
        memcpy(&record, base + PAT_OFFSET + pattern * sizeof(pattern_v5_t), sizeof(record));
        if (!pattern_core_valid(&record, false)) return false;
    }
    return true;
}
#endif

bool flash_storage_is_valid(void) {
#ifndef MOCK_HARDWARE
    const uint8_t *base = (const uint8_t *)(XIP_BASE + STORE_OFFSET);
    const store_header_t *h = (const store_header_t *)base;
    if (h->magic != MAGIC) return false;
    if (h->version == VERSION) {
        const pattern_t *patterns = (const pattern_t *)(base + PAT_OFFSET);
        const song_t *song = (const song_t *)(base + SONG_OFFSET);
        const kit_config_t *kits = (const kit_config_t *)(base + KIT_OFFSET);
        return payload_valid(patterns, song, kits) &&
               h->checksum == payload_checksum(patterns, song, kits);
    }
    if (h->version != V5_VERSION && h->version != LEGACY_VERSION) return false;
    if (!payload_valid_v5(base)) return false;
    // Version 4 had the same payload as 5, without a checksum.
    return h->version == LEGACY_VERSION ||
           h->checksum == payload_checksum_v5(base + PAT_OFFSET, base + V5_SONG_OFFSET);
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

    // Build padded pattern, song and kit buffers. Flash programming length
    // must be page-aligned, so never read beyond the object being serialized.
    memset(pattern_buf, 0xFF, sizeof(pattern_buf));

    static uint8_t song_buf[SONG_SIZE];
    memset(song_buf, 0xFF, sizeof(song_buf));

    static uint8_t kit_buf[KIT_SIZE];
    memset(kit_buf, 0xFF, sizeof(kit_buf));

    // seq_lock() keeps Core 0's interrupts off, the UART TX interrupt with them:
    // send what is still queued (the Stop and its releases) first, then copy one
    // location per lock section. Stopped, nothing else writes this data meanwhile.
    midi_wait_output_idle();
    for (uint8_t pattern = 0; pattern < MAX_PATTERNS; pattern++) {
        uint32_t lock_state = seq_lock();
        memcpy(pattern_buf + pattern * sizeof(pattern_t), &g_seq.patterns[pattern],
               sizeof(pattern_t));
        seq_unlock(lock_state);
    }
    uint32_t lock_state = seq_lock();
    memcpy(song_buf, &g_seq.song, SONG_BYTES);
    memcpy(kit_buf, g_seq.kit_config, KIT_BYTES);
    seq_unlock(lock_state);
    hdr.checksum = payload_checksum(pattern_buf, song_buf, kit_buf);

    // Pause Core 1 so it won't execute from flash during the write
    // Finish any Stop/Note Off bytes before UART interrupts are suspended.
    midi_wait_output_idle();
    multicore_lockout_start_blocking();
    // Until Core 1 was parked, its timer ISR could still queue an overflow
    // panic. Send that too while the UART interrupt can still run.
    midi_wait_output_idle();
    uint32_t ints = save_and_disable_interrupts();

    flash_range_erase(STORE_OFFSET, STORE_ERASE);

    flash_range_program(STORE_OFFSET + PAT_OFFSET,
                        pattern_buf, PAT_SIZE);

    flash_range_program(STORE_OFFSET + SONG_OFFSET,
                        song_buf, SONG_SIZE);

    flash_range_program(STORE_OFFSET + KIT_OFFSET,
                        kit_buf, KIT_SIZE);

    // Mark this save valid only after all payloads have been programmed.
    flash_range_program(STORE_OFFSET + HDR_OFFSET,
                        (const uint8_t *)&hdr, HDR_SIZE);

    restore_interrupts(ints);
    multicore_lockout_end_blocking();
    // Whether or not the new save validates.
    resync_inputs();
    return flash_storage_is_valid();
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
    bool v6 = h->version == VERSION;
    // As in the save: MIDI idle first, then one location per lock section.
    midi_wait_output_idle();
    for (uint8_t pattern = 0; pattern < MAX_PATTERNS; pattern++) {
        uint32_t lock_state = seq_lock();
        if (v6) {
            memcpy(&g_seq.patterns[pattern], base + PAT_OFFSET + pattern * sizeof(pattern_t),
                   sizeof(pattern_t));
        } else {
            // Legacy save: every record becomes the start of a zeroed pattern_t
            // (no kit steps).
            memset(&g_seq.patterns[pattern], 0, sizeof(pattern_t));
            memcpy(&g_seq.patterns[pattern],
                   base + PAT_OFFSET + pattern * sizeof(pattern_v5_t), sizeof(pattern_v5_t));
        }
        seq_unlock(lock_state);
    }
    uint32_t lock_state = seq_lock();
    if (v6) {
        memcpy(&g_seq.song, base + SONG_OFFSET, sizeof(g_seq.song));
        memcpy(g_seq.kit_config, base + KIT_OFFSET, sizeof(g_seq.kit_config));
    } else {
        // Legacy save: every track gets default kit settings.
        memcpy(&g_seq.song, base + V5_SONG_OFFSET, sizeof(g_seq.song));
        for (uint8_t track = 0; track < NUM_TRACKS; track++) {
            kit_config_defaults(&g_seq.kit_config[track]);
        }
    }
    seq_unlock(lock_state);
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

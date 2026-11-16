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

// ─── Flash layout ──────────────────────────────────────────────
//
// Last 128 KB of the 2 MB Pico flash is reserved for storage:
//
//   Byte 0        : Header (1 page = 256 bytes)
//   Byte 256      : Patterns (4 banks × 8 slots × sizeof(pattern_t))
//   Byte 256+PAT  : Song (padded to page boundary)
//
// RP2040 flash constants (always these values for the Pico):
#define FLASH_PAGE      256u
#define FLASH_SECTOR    4096u

#define FLASH_SZ        (2u * 1024u * 1024u)
#define STORE_RESERVED  (128u * 1024u)
#define STORE_OFFSET    (FLASH_SZ - STORE_RESERVED)   // raw flash offset
#define STORE_ERASE     STORE_RESERVED                 // erase the full 128 KB

#define HDR_OFFSET      0u
#define HDR_SIZE        FLASH_PAGE

#define PAT_OFFSET      HDR_SIZE
#define PAT_BYTES       (sizeof(pattern_t) * MAX_PATTERNS)
// Round up to next page boundary
#define PAT_SIZE        (((PAT_BYTES) + FLASH_PAGE - 1u) & ~(FLASH_PAGE - 1u))

#define SONG_OFFSET     (PAT_OFFSET + PAT_SIZE)
#define SONG_BYTES      sizeof(song_t)
#define SONG_SIZE       (((SONG_BYTES) + FLASH_PAGE - 1u) & ~(FLASH_PAGE - 1u))

// Sanity: total storage fits in reserved region
_Static_assert(HDR_SIZE + PAT_SIZE + SONG_SIZE <= STORE_RESERVED,
               "flash storage too large for reserved region");

// ─── Header ────────────────────────────────────────────────────

#define MAGIC   0x52425448u     // 'RBTH'
#define VERSION 5u
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

static uint32_t payload_checksum(const void *patterns, const void *song) {
    return ~crc32_update(crc32_update(UINT32_MAX, patterns, PAT_BYTES), song, SONG_BYTES);
}

static bool payload_valid(const pattern_t *patterns, const song_t *song) {
    if (song->length > MAX_SONG_ENTRIES || song->name[8] != '\0') return false;
    if (song->end_mode != SONG_END_LOOP && song->end_mode != SONG_END_STOP) return false;
    for (uint16_t entry = 0; entry < song->length; entry++) {
        for (uint8_t track = 0; track < NUM_TRACKS; track++) {
            if (song->entries[entry].pattern_index[track] >= MAX_PATTERNS) return false;
        }
    }
    for (uint8_t pattern = 0; pattern < MAX_PATTERNS; pattern++) {
        const pattern_t *p = &patterns[pattern];
        if (p->length < 1 || p->length > STEPS_PER_PATTERN ||
            p->swing > 99 || p->name[8] != '\0') return false;
        for (uint8_t track = 0; track < NUM_TRACKS; track++) {
            const track_config_t *tc = &p->tracks[track];
            uint8_t mute;
            memcpy(&mute, &tc->muted, sizeof(mute));
            if (mute > 1 || p->track_length[track] > STEPS_PER_PATTERN ||
                (tc->type != TRACK_TYPE_SYNTH && tc->type != TRACK_TYPE_DRUM) ||
                tc->midi_channel > 15 || tc->drum_note > 127 ||
                tc->output_mode < OUTPUT_MIDI_ONLY || tc->output_mode > OUTPUT_BOTH ||
                tc->cv_channel >= NUM_CV_CHANNELS ||
                tc->gate_length_pct < 10 || tc->gate_length_pct > 100) return false;
            for (uint8_t step = 0; step < STEPS_PER_PATTERN; step++) {
                const step_t *s = &p->steps[track][step];
                if (s->note > 127 || s->velocity > 127 || (s->flags & 0xF0u)) return false;
            }
        }
    }
    return true;
}
#endif

bool flash_storage_is_valid(void) {
#ifndef MOCK_HARDWARE
    const store_header_t *h =
        (const store_header_t *)(XIP_BASE + STORE_OFFSET);
    if (h->magic != MAGIC || (h->version != VERSION && h->version != LEGACY_VERSION)) return false;
    const uint8_t *base = (const uint8_t *)(XIP_BASE + STORE_OFFSET);
    const pattern_t *patterns = (const pattern_t *)(base + PAT_OFFSET);
    const song_t *song = (const song_t *)(base + SONG_OFFSET);
    if (!payload_valid(patterns, song)) return false;
    // Version 4 used the same payload layout, without a checksum.
    return h->version == LEGACY_VERSION || h->checksum == payload_checksum(patterns, song);
#else
    return false;
#endif
}

bool flash_storage_save(void) {
    // Storage and transport commands are issued by Core 0. Core 1 can stop
    // playback, but cannot start it while this synchronous operation runs.
    if (g_seq.transport.state != TRANSPORT_STOPPED) return false;
#ifndef MOCK_HARDWARE
    if (!payload_valid(g_seq.patterns, &g_seq.song)) return false;
    // Build the 256-byte header page
    static store_header_t hdr;
    memset(&hdr, 0xFF, sizeof(hdr));    // 0xFF = erased flash default
    hdr.magic   = MAGIC;
    hdr.version = (uint8_t)VERSION;

    // Build padded pattern and song buffers. Flash programming length must
    // be page-aligned, so never read beyond the object being serialized.
    static uint8_t pattern_buf[PAT_SIZE];
    memset(pattern_buf, 0xFF, sizeof(pattern_buf));

    // Build padded song buffer (song_t may not be page-aligned)
    static uint8_t song_buf[SONG_SIZE];
    memset(song_buf, 0xFF, sizeof(song_buf));

    uint32_t lock_state = seq_lock();
    memcpy(pattern_buf, g_seq.patterns, PAT_BYTES);
    memcpy(song_buf, &g_seq.song, SONG_BYTES);
    seq_unlock(lock_state);
    hdr.checksum = payload_checksum(pattern_buf, song_buf);

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

    // Mark this save valid only after both payloads have been programmed.
    flash_range_program(STORE_OFFSET + HDR_OFFSET,
                        (const uint8_t *)&hdr, HDR_SIZE);

    restore_interrupts(ints);
    multicore_lockout_end_blocking();
    // Keys held through the stall must not fire afterwards, whether or not
    // the new save validates.
    panel_input_resync();
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
    uint32_t lock_state = seq_lock();
    memcpy(g_seq.patterns, base + PAT_OFFSET,  sizeof(g_seq.patterns));
    memcpy(&g_seq.song,    base + SONG_OFFSET, sizeof(g_seq.song));
    seq_unlock(lock_state);
    for (uint8_t pattern = 0; pattern < MAX_PATTERNS; pattern++) {
        for (uint8_t track = 0; track < NUM_TRACKS; track++) {
            sequencer_refresh_track_playback(track, pattern);
        }
    }
    panel_input_resync();
    return true;
#else
    return false;
#endif
}

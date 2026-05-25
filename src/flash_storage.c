#ifndef MOCK_HARDWARE
#include "pico/stdlib.h"
#include "hardware/flash.h"
#include "hardware/sync.h"
#include "pico/multicore.h"
#endif

#include <string.h>
#include "sequencer.h"
#include "flash_storage.h"

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
#define VERSION 4u

typedef struct {
    uint32_t magic;
    uint8_t  version;
    uint8_t  _pad[FLASH_PAGE - 5u];    // Pad header to exactly one page
} store_header_t;

_Static_assert(sizeof(store_header_t) == FLASH_PAGE,
               "store_header_t must be exactly one flash page");

// ─── API ───────────────────────────────────────────────────────

bool flash_storage_is_valid(void) {
#ifndef MOCK_HARDWARE
    const store_header_t *h =
        (const store_header_t *)(XIP_BASE + STORE_OFFSET);
    return h->magic == MAGIC && h->version == VERSION;
#else
    return false;
#endif
}

bool flash_storage_save(void) {
#ifndef MOCK_HARDWARE
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

    // Pause Core 1 so it won't execute from flash during the write
    multicore_lockout_start_blocking();
    uint32_t ints = save_and_disable_interrupts();

    flash_range_erase(STORE_OFFSET, STORE_ERASE);

    flash_range_program(STORE_OFFSET + HDR_OFFSET,
                        (const uint8_t *)&hdr, HDR_SIZE);

    flash_range_program(STORE_OFFSET + PAT_OFFSET,
                        pattern_buf, PAT_SIZE);

    flash_range_program(STORE_OFFSET + SONG_OFFSET,
                        song_buf, SONG_SIZE);

    restore_interrupts(ints);
    multicore_lockout_end_blocking();
    return true;
#else
    return false;
#endif
}

bool flash_storage_load(void) {
#ifndef MOCK_HARDWARE
    if (!flash_storage_is_valid()) return false;

    const uint8_t *base = (const uint8_t *)(XIP_BASE + STORE_OFFSET);
    uint32_t lock_state = seq_lock();
    memcpy(g_seq.patterns, base + PAT_OFFSET,  sizeof(g_seq.patterns));
    memcpy(&g_seq.song,    base + SONG_OFFSET, sizeof(g_seq.song));
    seq_unlock(lock_state);
    return true;
#else
    return false;
#endif
}

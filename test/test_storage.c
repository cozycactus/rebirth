#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fakes/fake_hardware.h"
#include "../src/flash_storage.c"

sequencer_state_t g_seq;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%d: %s\n", __LINE__, #x); exit(1); } } while (0)

static unsigned resync_calls, mcp_resync_calls, encoder_resync_calls;
void panel_input_resync(void) { resync_calls++; }
void mcp23017_resync(void) { mcp_resync_calls++; }
void encoder_resync(void) { encoder_resync_calls++; }

// Every input is resynced together: panel keys, step buttons, encoder buttons.
static bool all_inputs_resynced(unsigned times) {
    return resync_calls == times && mcp_resync_calls == times && encoder_resync_calls == times;
}

static void test_save_resyncs_panel_input(void) {
    resync_calls = mcp_resync_calls = encoder_resync_calls = 0;
    CHECK(flash_storage_save());
    CHECK(all_inputs_resynced(1));
    // A refused save (playing) must not resync.
    transport_start();
    CHECK(!flash_storage_save());
    transport_stop();
    CHECK(all_inputs_resynced(1));
    // A save that reached the lockout resyncs even if it fails validation.
    resync_calls = mcp_resync_calls = encoder_resync_calls = 0;
    fake_flash_programs = 0;
    fake_flash_program_limit = 2;
    CHECK(!flash_storage_save());
    fake_flash_program_limit = UINT32_MAX;
    CHECK(all_inputs_resynced(1));
    CHECK(flash_storage_save());
}

static void test_load_resyncs_panel_input(void) {
    resync_calls = mcp_resync_calls = encoder_resync_calls = 0;
    CHECK(flash_storage_load());
    CHECK(all_inputs_resynced(1));
    fake_flash[STORE_OFFSET + PAT_OFFSET] ^= 1;
    CHECK(!flash_storage_load());
    fake_flash[STORE_OFFSET + PAT_OFFSET] ^= 1;
    CHECK(all_inputs_resynced(1));
}

// Core 1's timer ISR clears a MIDI overflow with a panic even while stopped.
// Here that happens after the save's first drain, before Core 1 is parked.
static void core1_panics_before_lockout(void) {
    sequencer_panic();
}

int main(void) {
    seq_lock_init();
    midi_init();
    g_seq.transport.bpm = DEFAULT_BPM;
    for (uint8_t pattern = 0; pattern < MAX_PATTERNS; pattern++) {
        pattern_init(&g_seq.patterns[pattern], pattern);
    }
    sequencer_init();
    pattern_set_step(&g_seq.patterns[7], 2, 0, 72, 110, STEP_FLAG_ACTIVE);
    CHECK(song_append(7, 2));
    CHECK(!flash_storage_is_valid());
    CHECK(flash_storage_save());
    CHECK(fake_flash_last_program_offset == STORE_OFFSET + HDR_OFFSET);
    CHECK(flash_storage_is_valid());
    pattern_clear(&g_seq.patterns[7]);
    song_clear();
    CHECK(flash_storage_load());
    CHECK(g_seq.patterns[7].steps[2][0].note == 72);
    CHECK(g_seq.song.length == 1);
    CHECK(g_seq.song.entries[0].pattern_index[2] == 7);

    transport_start();
    unsigned erases = fake_flash_erases;
    CHECK(!flash_storage_save());
    CHECK(fake_flash_erases == erases);
    CHECK(!flash_storage_load());
    transport_stop();

    // A bit flip in an otherwise valid note must fail the checksum.
    fake_flash[STORE_OFFSET + PAT_OFFSET] ^= 1;
    CHECK(!flash_storage_load());
    CHECK(g_seq.patterns[7].steps[2][0].note == 72);
    fake_flash[STORE_OFFSET + PAT_OFFSET] ^= 1;

    store_header_t *header = (store_header_t *)(fake_flash + STORE_OFFSET);
    header->version = LEGACY_VERSION;
    CHECK(flash_storage_load());
    song_t *saved_song = (song_t *)(fake_flash + STORE_OFFSET + SONG_OFFSET);
    saved_song->length = MAX_SONG_ENTRIES + 1;
    CHECK(!flash_storage_load());
    CHECK(g_seq.song.length == 1);
    saved_song->length = 1;
    saved_song->entries[0].pattern_index[0] = MAX_PATTERNS;
    CHECK(!flash_storage_load());

    // Simulate interruption after payload pages, before the commit header.
    fake_flash_programs = 0;
    fake_flash_program_limit = 2;
    CHECK(!flash_storage_save());
    CHECK(!flash_storage_is_valid());
    CHECK(!flash_storage_load());
    fake_flash_program_limit = UINT32_MAX;
    CHECK(flash_storage_save());
    CHECK(header->version == VERSION);

    // MIDI queued by Core 1 just before the lockout is sent before the erase.
    fake_before_lockout = core1_panics_before_lockout;
    CHECK(flash_storage_save());
    fake_before_lockout = NULL;
    test_save_resyncs_panel_input();
    test_load_resyncs_panel_input();
    puts("PASS storage round-trip, playback guard, checksum, legacy validation, "
         "interrupted save, late MIDI before erase, resync after save and load");
    return 0;
}

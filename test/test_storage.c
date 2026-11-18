#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fakes/fake_hardware.h"
#include "../src/flash_storage.c"
#include "kit.h"

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


// ─── Image helpers ─────────────────────────────────────────────

#define IMG (fake_flash + STORE_OFFSET)

// Serialize the RAM patterns and song as a version 4, 5 or 6 image.
static void write_legacy_image(uint8_t version) {
    memset(IMG, 0xff, STORE_ERASE);
    uint8_t *old = fake_flash + OLD_STORE;
    memset(old, 0xff, 128u * 1024);
    for (uint8_t i = 0; i < MAX_PATTERNS; i++) {
        const pattern_t *p = &g_seq.patterns[i];
        pattern_v6_t record = {0};
        memcpy(record.steps, p->steps, sizeof(record.steps));
        memcpy(record.tracks, p->tracks, sizeof(record.tracks));
        memcpy(record.track_length, p->track_length, sizeof(record.track_length));
        memcpy(record.name, p->name, sizeof(record.name));
        record.swing = p->swing;
        record.length = p->length;
        memcpy(record.kit_steps, p->kit_steps, sizeof(record.kit_steps));
        size_t stride = version == 6 ? sizeof(record) : sizeof(pattern_v5_t);
        memcpy(old + PAT_OFFSET + i * stride, &record, stride);
    }
    old_song_t song = {0};
    song.length = g_seq.song.length;
    song.end_mode = g_seq.song.end_mode;
    memcpy(song.name, g_seq.song.name, sizeof(song.name));
    for (unsigned i = 0; i < song.length; i++) {
        memcpy(song.entries[i].pattern_index, g_seq.song.entries[i].pattern_index, OLD_TRACKS);
        song.entries[i].repeats = g_seq.song.entries[i].repeats;
    }
    memcpy(old + (version == 6 ? V6_SONG_OFFSET : V5_SONG_OFFSET), &song, sizeof(song));
    store_header_t *header = (store_header_t *)old;
    header->magic = MAGIC;
    header->version = version;
    if (version == 5) header->checksum = payload_checksum_v5(old + PAT_OFFSET, &song);
    if (version == 6) {
        const size_t bytes = OLD_TRACKS * sizeof(kit_config_t);
        memcpy(old + V6_KIT_OFFSET, g_seq.kit_config, bytes);
        uint32_t crc = crc32_update(UINT32_MAX, old + PAT_OFFSET, V6_PAT_BYTES);
        crc = crc32_update(crc, &song, sizeof(song));
        header->checksum = ~crc32_update(crc, old + V6_KIT_OFFSET, bytes);
    }
}

// Recompute the current checksum after a test edited the image.
static void reseal_v7(void) {
    store_header_t *header = (store_header_t *)IMG;
    header->checksum = payload_checksum(IMG + PAT_OFFSET, IMG + SONG_OFFSET, IMG + KIT_OFFSET);
}

static pattern_t *img_patterns(void) { return (pattern_t *)(IMG + PAT_OFFSET); }
static kit_config_t *img_kits(void) { return (kit_config_t *)(IMG + KIT_OFFSET); }

typedef struct {
    pattern_t patterns[MAX_PATTERNS];
    song_t song;
    kit_config_t kits[NUM_TRACKS];
} ram_snapshot_t;

static ram_snapshot_t snap;
static void ram_take(void) {
    memcpy(snap.patterns, g_seq.patterns, sizeof(snap.patterns));
    memcpy(&snap.song, &g_seq.song, sizeof(snap.song));
    memcpy(snap.kits, g_seq.kit_config, sizeof(snap.kits));
}
static bool ram_unchanged(void) {
    return memcmp(snap.patterns, g_seq.patterns, sizeof(snap.patterns)) == 0 &&
           memcmp(&snap.song, &g_seq.song, sizeof(snap.song)) == 0 &&
           memcmp(snap.kits, g_seq.kit_config, sizeof(snap.kits)) == 0;
}
static void ram_restore(void) {
    memcpy(g_seq.patterns, snap.patterns, sizeof(snap.patterns));
    memcpy(&g_seq.song, &snap.song, sizeof(snap.song));
    memcpy(g_seq.kit_config, snap.kits, sizeof(snap.kits));
}

static void set_kit_tracks(uint8_t track_a, uint8_t track_b) {
    for (uint8_t pattern = 0; pattern < MAX_PATTERNS; pattern++) {
        g_seq.patterns[pattern].tracks[track_a].type = TRACK_TYPE_KIT;
        g_seq.patterns[pattern].tracks[track_b].type = TRACK_TYPE_KIT;
    }
}

static void test_v7_round_trip(void) {
    set_kit_tracks(1, 4);
    g_seq.patterns[5].kit_steps[1][3] = kit_set(0, 2, 3);
    g_seq.patterns[31].kit_steps[4][15] = kit_set(kit_set(0, 0, 1), 7, 2);
    kit_config_t cfg;
    kit_config_defaults(&cfg);
    cfg.note[0] = 40;
    cfg.vel_normal = 70;
    cfg.flam_pulses = 5;
    seq_set_kit_config(1, &cfg);
    pattern_set_step(&g_seq.patterns[23], 63, 15, 74, 99, STEP_FLAG_ACTIVE);
    g_seq.song.entries[0].pattern_index[63] = 23;
    CHECK(flash_storage_save());
    CHECK(((store_header_t *)IMG)->version == VERSION);
    ram_take();
    memset(g_seq.patterns, 0, sizeof(g_seq.patterns));
    memset(g_seq.kit_config, 0, sizeof(g_seq.kit_config));
    song_clear();
    CHECK(flash_storage_load());
    CHECK(g_seq.patterns[5].kit_steps[1][3] == kit_set(0, 2, 3));
    CHECK(g_seq.patterns[5].tracks[1].type == TRACK_TYPE_KIT);
    CHECK(g_seq.patterns[0].tracks[4].type == TRACK_TYPE_KIT);
    CHECK(g_seq.kit_config[1].note[0] == 40);
    CHECK(g_seq.kit_config[1].vel_normal == 70);
    CHECK(g_seq.kit_config[1].flam_pulses == 5);
    CHECK(g_seq.song.entries[0].pattern_index[63] == 23);
    CHECK(g_seq.patterns[23].steps[63][15].note == 74);
    CHECK(memcmp(snap.patterns, g_seq.patterns, sizeof(snap.patterns)) == 0);
    CHECK(memcmp(snap.kits, g_seq.kit_config, sizeof(snap.kits)) == 0);
}

static void expect_legacy_load(uint8_t version) {
    // Start from a kit-free state with a recognizable song and steps.
    for (uint8_t pattern = 0; pattern < MAX_PATTERNS; pattern++) {
        for (uint8_t track = 0; track < NUM_TRACKS; track++) {
            g_seq.patterns[pattern].tracks[track].type =
                track == 5 ? TRACK_TYPE_DRUM : TRACK_TYPE_SYNTH;
            if (track < KIT_TRACK_SLOTS) memset(g_seq.patterns[pattern].kit_steps[track], 0,
                   sizeof(g_seq.patterns[pattern].kit_steps[track]));
        }
    }
    pattern_set_step(&g_seq.patterns[9], 3, 4, 61, 99, STEP_FLAG_ACTIVE);
    g_seq.patterns[9].swing = 40;
    song_clear();
    CHECK(song_append(9, 3));
    CHECK(song_append(2, 1));
    ram_take();
    write_legacy_image(version);
    // Garbage the kit data in RAM so the load must replace it.
    set_kit_tracks(0, 1);
    for (uint8_t pattern = 0; pattern < MAX_PATTERNS; pattern++) {
        g_seq.patterns[pattern].kit_steps[0][0] = 0x55;
        g_seq.patterns[pattern].tracks[0].type = TRACK_TYPE_SYNTH;
        g_seq.patterns[pattern].tracks[1].type = TRACK_TYPE_SYNTH;
    }
    memset(g_seq.kit_config, 0, sizeof(g_seq.kit_config));
    CHECK(flash_storage_is_valid());
    CHECK(flash_storage_load());
    for (uint8_t pattern = 0; pattern < MAX_PATTERNS; pattern++) {
        CHECK(memcmp(&g_seq.patterns[pattern], &snap.patterns[pattern],
                     sizeof(pattern_v5_t)) == 0);
        for (uint8_t track = 0; track < KIT_TRACK_SLOTS; track++) {
            for (uint8_t step = 0; step < STEPS_PER_PATTERN; step++) {
                CHECK(g_seq.patterns[pattern].kit_steps[track][step] == 0);
            }
        }
    }
    CHECK(g_seq.patterns[9].steps[3][4].note == 61);
    CHECK(g_seq.song.length == snap.song.length && g_seq.song.end_mode == snap.song.end_mode);
    for (unsigned i = 0; i < snap.song.length; i++) {
        CHECK(g_seq.song.entries[i].repeats == snap.song.entries[i].repeats);
        CHECK(!memcmp(g_seq.song.entries[i].pattern_index, snap.song.entries[i].pattern_index, OLD_TRACKS));
        for (unsigned t = OLD_TRACKS; t < NUM_TRACKS; t++) CHECK(g_seq.song.entries[i].pattern_index[t] == 0);
    }
    kit_config_t defaults;
    kit_config_defaults(&defaults);
    for (uint8_t track = 0; track < NUM_TRACKS; track++) {
        CHECK(memcmp(&g_seq.kit_config[track], &defaults, sizeof(defaults)) == 0);
    }
}

static void test_legacy_images(void) {
    set_kit_tracks(1, 4);
    g_seq.patterns[31].kit_steps[4][15] = kit_set(0, 7, 2);
    g_seq.kit_config[1].note[0] = 42;
    write_legacy_image(6);
    g_seq.patterns[31].kit_steps[4][15] = 0;
    g_seq.kit_config[1].note[0] = 0;
    CHECK(flash_storage_load());
    CHECK(g_seq.patterns[31].tracks[4].type == TRACK_TYPE_KIT);
    CHECK(g_seq.patterns[31].kit_steps[4][15] == kit_set(0, 7, 2));
    CHECK(g_seq.kit_config[1].note[0] == 42);
    CHECK(!(g_seq.patterns[23].steps[63][15].flags & STEP_FLAG_ACTIVE));
    (fake_flash + OLD_STORE)[V6_KIT_OFFSET] ^= 1;
    CHECK(!flash_storage_load());
    expect_legacy_load(V5_VERSION);
    // A flipped byte fails the v5 checksum.
    write_legacy_image(V5_VERSION);
    (fake_flash + OLD_STORE)[PAT_OFFSET + 3 * sizeof(pattern_v5_t)] ^= 1;
    ram_take();
    CHECK(!flash_storage_is_valid());
    CHECK(!flash_storage_load());
    CHECK(ram_unchanged());
    // A v4 image has no checksum: any checksum field loads.
    write_legacy_image(LEGACY_VERSION);
    ((store_header_t *)(fake_flash + OLD_STORE))->checksum = 0x12345678u;
    CHECK(flash_storage_load());
    expect_legacy_load(LEGACY_VERSION);
    // A kit type is not valid in a legacy image.
    write_legacy_image(V5_VERSION);
    ((pattern_v5_t *)((fake_flash + OLD_STORE) + PAT_OFFSET))[2].tracks[1].type = TRACK_TYPE_KIT;
    ((store_header_t *)(fake_flash + OLD_STORE))->checksum =
        payload_checksum_v5((fake_flash + OLD_STORE) + PAT_OFFSET, (fake_flash + OLD_STORE) + V5_SONG_OFFSET);
    CHECK(!flash_storage_is_valid());
    // A song past the limit in a v4 image is rejected.
    write_legacy_image(LEGACY_VERSION);
    ((old_song_t *)((fake_flash + OLD_STORE) + V5_SONG_OFFSET))->length = MAX_SONG_ENTRIES + 1;
    CHECK(!flash_storage_load());
}

enum { BAD_BIT24, BAD_AC2, BAD_NOTE, BAD_VEL0, BAD_FLAM13, BAD_THREE_KITS, BAD_COUNT };

static void apply_bad(int which, pattern_t *patterns, kit_config_t *kits) {
    switch (which) {
    case BAD_BIT24: patterns[3].kit_steps[1][2] = 1u << 24; break;
    case BAD_AC2:   patterns[3].kit_steps[4][0] = 2; break;
    case BAD_NOTE:  kits[1].note[3] = 200; break;
    case BAD_VEL0:  kits[4].vel_normal = 0; break;
    case BAD_FLAM13: kits[1].flam_pulses = 13; break;
    case BAD_THREE_KITS:
        patterns[0].tracks[2].type = TRACK_TYPE_KIT;
        patterns[1].tracks[6].type = TRACK_TYPE_KIT;
        patterns[2].tracks[7].type = TRACK_TYPE_KIT;
        break;
    }
}

static void test_v6_rejections(void) {
    for (int which = 0; which < BAD_COUNT; which++) {
        // Kits on tracks 1 and 4 in all 32 locations, except the three-kit case.
        set_kit_tracks(1, 4);
        if (which == BAD_THREE_KITS) {
            for (uint8_t pattern = 0; pattern < MAX_PATTERNS; pattern++) {
                g_seq.patterns[pattern].tracks[4].type = TRACK_TYPE_SYNTH;
            }
        }
        CHECK(flash_storage_save());
        ram_take();
        apply_bad(which, img_patterns(), img_kits());
        reseal_v7();
        CHECK(!flash_storage_is_valid());
        CHECK(!flash_storage_load());
        CHECK(ram_unchanged());
        // Saving the same data from RAM is refused, and flash is not touched.
        unsigned erases = fake_flash_erases;
        apply_bad(which, g_seq.patterns, g_seq.kit_config);
        CHECK(!flash_storage_save());
        CHECK(fake_flash_erases == erases);
        ram_restore();
    }
    // Tracks 1 and 4 hold the same kit in every location: still valid.
    set_kit_tracks(1, 4);
    CHECK(flash_storage_save());
    // Kits spread over two tracks, with a different track each location: three in total.
    g_seq.patterns[0].tracks[2].type = TRACK_TYPE_KIT;
    CHECK(!flash_storage_save());
    set_kit_tracks(1, 4);
}

// ─── Short lock sections ───────────────────────────────────────
// seq_lock() keeps Core 0's interrupts off, the UART TX interrupt with them. The save
// and the load copy one pattern per section, and only once MIDI output is idle. Every
// spin lock taken marks a section boundary here: between two, at most one of the
// MAX_PATTERNS records watched may change.

static uint8_t *watched;
static uint8_t watched_before[PAT_BYTES];
static bool watch_started;
static unsigned most_changed;               // most records changed between two locks
static unsigned seq_sections_midi_pending;  // seq_lock sections entered with MIDI pending

static void watch_lock(spin_lock_t *lock) {
    fake_uart_writable = true;              // the wire frees up while the caller waits
    if (lock == spin_lock_instance(g_seq.spinlock_num) && fake_uart_tx_irq) {
        seq_sections_midi_pending++;
    }
    if (watch_started) {
        unsigned changed = 0;
        for (unsigned p = 0; p < MAX_PATTERNS; p++) {
            changed += memcmp(watched + p * sizeof(pattern_t),
                              watched_before + p * sizeof(pattern_t), sizeof(pattern_t)) != 0;
        }
        if (changed > most_changed) most_changed = changed;
    }
    memcpy(watched_before, watched, PAT_BYTES);
    watch_started = true;
}

// Watches `records`, with a Note On still queued behind a busy wire.
static void watch(void *records) {
    watched = records;
    watch_started = false;
    most_changed = 0;
    seq_sections_midi_pending = 0;
    fake_before_lock = NULL;
    fake_uart_writable = false;
    midi_send_note_on(0, 60, 100);
    CHECK(fake_uart_tx_irq);
    fake_before_lock = watch_lock;
}

// Changes every location in RAM, so a load overwrites each.
static void change_every_location(void) {
    for (uint8_t pattern = 0; pattern < MAX_PATTERNS; pattern++) {
        g_seq.patterns[pattern].swing = 77;
        g_seq.patterns[pattern].kit_steps[7][15] = 0x55;
    }
}

static void test_copies_one_pattern_per_lock_section(void) {
    for (uint8_t pattern = 0; pattern < MAX_PATTERNS; pattern++) {
        pattern_init(&g_seq.patterns[pattern], pattern);
        pattern_set_step(&g_seq.patterns[pattern], 0, pattern % 16, 40 + pattern, 90,
                         STEP_FLAG_ACTIVE);
    }
    song_clear();
    CHECK(song_append(3, 2));

    watch(g_seq.patterns);
    CHECK(flash_storage_save());
    CHECK(most_changed == 0);
    CHECK(seq_sections_midi_pending == 0);
    ram_take();

    change_every_location();
    watch(g_seq.patterns);
    CHECK(flash_storage_load());
    CHECK(most_changed == 1);
    CHECK(seq_sections_midi_pending == 0);
    CHECK(ram_unchanged());

    write_legacy_image(V5_VERSION);
    change_every_location();
    watch(g_seq.patterns);
    CHECK(flash_storage_load());
    CHECK(most_changed == 1);
    CHECK(seq_sections_midi_pending == 0);
    CHECK(g_seq.song.length == snap.song.length && g_seq.song.end_mode == snap.song.end_mode);
    for (unsigned i = 0; i < snap.song.length; i++) {
        CHECK(g_seq.song.entries[i].repeats == snap.song.entries[i].repeats);
        CHECK(!memcmp(g_seq.song.entries[i].pattern_index, snap.song.entries[i].pattern_index, OLD_TRACKS));
        for (unsigned t = OLD_TRACKS; t < NUM_TRACKS; t++) CHECK(g_seq.song.entries[i].pattern_index[t] == 0);
    }
    for (uint8_t pattern = 0; pattern < MAX_PATTERNS; pattern++) {
        CHECK(memcmp(&g_seq.patterns[pattern], &snap.patterns[pattern], sizeof(pattern_v5_t)) == 0);
        CHECK(g_seq.patterns[pattern].kit_steps[7][15] == 0);
    }
    fake_before_lock = NULL;
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
    write_legacy_image(LEGACY_VERSION);
    CHECK(flash_storage_load());
    old_song_t *saved_song = (old_song_t *)(fake_flash + OLD_STORE + V5_SONG_OFFSET);
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
    test_v7_round_trip();
    test_legacy_images();
    test_v6_rejections();
    test_copies_one_pattern_per_lock_section();
    puts("PASS storage round-trip, playback guard, checksum, legacy validation, "
         "interrupted save, late MIDI before erase, resync after save and load, "
         "v6 kits, v4/v5 migration, v6 rejections, one pattern per lock section");
    return 0;
}

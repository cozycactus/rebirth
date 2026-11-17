#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "midi_in.h"

static int failures;
#define CHECK(cond) do { if (!(cond)) { \
    printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

// Feeds the bytes and collects the events they complete.
static unsigned feed(midi_in_parser_t *p, const uint8_t *bytes, unsigned n,
                     midi_in_event_t *events, unsigned max) {
    unsigned count = 0;
    for (unsigned i = 0; i < n; i++) {
        midi_in_event_t e;
        if (midi_in_parse(p, bytes[i], &e) && count < max) events[count++] = e;
    }
    return count;
}

#define FEED(p, events, ...) \
    feed((p), (const uint8_t[]){__VA_ARGS__}, sizeof((const uint8_t[]){__VA_ARGS__}), (events), 16)

static void test_note_on_and_off_on_every_channel(void) {
    for (uint8_t ch = 0; ch < 16; ch++) {
        midi_in_parser_t p;
        midi_in_event_t e[16];
        midi_in_parser_reset(&p);
        CHECK(FEED(&p, e, 0x90 | ch, 60, 100) == 1);
        CHECK(e[0].kind == MIDI_IN_NOTE_ON && e[0].channel == ch);
        CHECK(e[0].note == 60 && e[0].velocity == 100);
        CHECK(FEED(&p, e, 0x80 | ch, 61, 33) == 1);
        CHECK(e[0].kind == MIDI_IN_NOTE_OFF && e[0].channel == ch);
        CHECK(e[0].note == 61 && e[0].velocity == 33);
    }
}

static void test_note_on_velocity_zero_is_note_off(void) {
    midi_in_parser_t p;
    midi_in_event_t e[16];
    midi_in_parser_reset(&p);
    CHECK(FEED(&p, e, 0x93, 64, 0) == 1);
    CHECK(e[0].kind == MIDI_IN_NOTE_OFF && e[0].channel == 3 && e[0].note == 64);
}

static void test_running_status(void) {
    midi_in_parser_t p;
    midi_in_event_t e[16];
    midi_in_parser_reset(&p);
    CHECK(FEED(&p, e, 0x90, 60, 100, 62, 90, 64, 80) == 3);
    CHECK(e[0].note == 60 && e[1].note == 62 && e[2].note == 64);
    CHECK(e[1].velocity == 90 && e[2].velocity == 80);
    CHECK(e[2].kind == MIDI_IN_NOTE_ON && e[2].channel == 0);
}

static void test_realtime_bytes_change_nothing(void) {
    midi_in_parser_t p;
    midi_in_event_t e[16];
    midi_in_parser_reset(&p);
    CHECK(FEED(&p, e, 0x90, 0xF8, 60, 0xFE, 100) == 1);
    CHECK(e[0].note == 60 && e[0].velocity == 100);
    CHECK(FEED(&p, e, 0xFE, 0xF8, 0xF9, 0xFA, 0xFB, 0xFC, 0xFD) == 0);
    // Running status survives them, and so does a message cut between data bytes.
    CHECK(FEED(&p, e, 62, 0xFE, 90) == 1);
    CHECK(e[0].note == 62 && e[0].velocity == 90);
    CHECK(FEED(&p, e, 0x80, 0xF8, 62, 0xF8, 0) == 1);
    CHECK(e[0].kind == MIDI_IN_NOTE_OFF && e[0].note == 62);
}

static void test_system_reset_resets_the_parser(void) {
    midi_in_parser_t p;
    midi_in_event_t e[16];
    midi_in_parser_reset(&p);
    CHECK(FEED(&p, e, 0x90, 0x3C, 0xFF, 0x40, 0x64) == 0);
    // The status is gone too, not only the half message.
    CHECK(FEED(&p, e, 0x90, 0x3C, 0x64, 0xFF, 0x3C, 0x64) == 1);
}

static void test_other_channel_messages_produce_nothing(void) {
    midi_in_parser_t p;
    midi_in_event_t e[16];
    midi_in_parser_reset(&p);
    CHECK(FEED(&p, e, 0xB0, 7, 100) == 0);          // volume
    CHECK(FEED(&p, e, 0xE0, 0, 64) == 0);           // pitch bend
    CHECK(FEED(&p, e, 0xA0, 60, 50) == 0);          // key pressure
    CHECK(FEED(&p, e, 0xD0, 50) == 0);              // channel pressure
    CHECK(FEED(&p, e, 0x90, 60, 100) == 1);         // the note after them parses
    CHECK(FEED(&p, e, 0xB0, 1, 2, 0xB0, 119, 0, 0xB0, 121, 0, 0xB0, 124, 0) == 0);
    CHECK(FEED(&p, e, 0x91, 61, 90) == 1);
    CHECK(e[0].channel == 1 && e[0].note == 61);
}

static void test_program_change_keeps_its_running_status(void) {
    midi_in_parser_t p;
    midi_in_event_t e[16];
    midi_in_parser_reset(&p);
    // After C0 05 the bytes 3C 64 are two more program changes.
    CHECK(FEED(&p, e, 0xC0, 0x05, 0x3C, 0x64) == 0);
    CHECK(FEED(&p, e, 0x90, 0x3C, 0x64) == 1);
    CHECK(e[0].kind == MIDI_IN_NOTE_ON && e[0].note == 0x3C && e[0].velocity == 0x64);
}

static void test_all_sound_off_and_all_notes_off(void) {
    for (uint8_t ch = 0; ch < 16; ch++) {
        midi_in_parser_t p;
        midi_in_event_t e[16];
        midi_in_parser_reset(&p);
        CHECK(FEED(&p, e, 0xB0 | ch, 120, 0) == 1);
        CHECK(e[0].kind == MIDI_IN_ALL_OFF && e[0].channel == ch);
        CHECK(FEED(&p, e, 0xB0 | ch, 123, 0) == 1);
        CHECK(e[0].kind == MIDI_IN_ALL_OFF);
    }
    // Under running status as well.
    midi_in_parser_t p;
    midi_in_event_t e[16];
    midi_in_parser_reset(&p);
    CHECK(FEED(&p, e, 0xB0, 7, 100, 123, 0) == 1);
    CHECK(e[0].kind == MIDI_IN_ALL_OFF);
}

static void test_sysex_is_skipped_and_ends_running_status(void) {
    midi_in_parser_t p;
    midi_in_event_t e[16];
    midi_in_parser_reset(&p);
    CHECK(FEED(&p, e, 0x90, 60, 100) == 1);
    // A body that looks like notes, then the end byte.
    CHECK(FEED(&p, e, 0xF0, 0x7E, 0x3C, 0x64, 0x3D, 0x64, 0xF7) == 0);
    // The status from before the SysEx is not used.
    CHECK(FEED(&p, e, 0x3E, 0x64) == 0);
    CHECK(FEED(&p, e, 0x90, 0x3E, 0x64) == 1);
}

static void test_system_common_drops_a_note_in_progress(void) {
    midi_in_parser_t p;
    midi_in_event_t e[16];
    midi_in_parser_reset(&p);
    CHECK(FEED(&p, e, 0x90, 0x3C, 0xF2, 0x00, 0x00, 0x40, 0x64) == 0);
    CHECK(FEED(&p, e, 0x90, 0x3C, 0xF1, 0x05, 0x40, 0x64) == 0);
}

static void test_data_bytes_without_a_status_are_ignored(void) {
    midi_in_parser_t p;
    midi_in_event_t e[16];
    midi_in_parser_reset(&p);
    CHECK(FEED(&p, e, 0x3C, 0x64, 0x3C, 0x64) == 0);
    CHECK(FEED(&p, e, 0x90, 0x3C, 0x64) == 1);
}

static void test_a_new_status_drops_the_partial_message(void) {
    midi_in_parser_t p;
    midi_in_event_t e[16];
    midi_in_parser_reset(&p);
    CHECK(FEED(&p, e, 0x90, 0x3C, 0x91, 0x40, 0x50) == 1);
    CHECK(e[0].channel == 1 && e[0].note == 0x40 && e[0].velocity == 0x50);
    CHECK(FEED(&p, e, 0x91, 0x40, 0x80, 0x41, 0x00) == 1);
    CHECK(e[0].kind == MIDI_IN_NOTE_OFF && e[0].channel == 0 && e[0].note == 0x41);
    // The half message started under the old status leaves no data behind.
    CHECK(FEED(&p, e, 0x90, 0x3C, 0x90, 0x40, 0x64) == 1);
    CHECK(e[0].note == 0x40);
}

static void test_reset_ignores_data_until_a_status(void) {
    midi_in_parser_t p;
    midi_in_event_t e[16];
    midi_in_parser_reset(&p);
    CHECK(FEED(&p, e, 0x90, 0x3C, 0x64) == 1);
    midi_in_parser_reset(&p);
    CHECK(FEED(&p, e, 0x3C, 0x64) == 0);
    CHECK(FEED(&p, e, 0x3C) == 0);
    midi_in_parser_reset(&p);
    CHECK(FEED(&p, e, 0x90, 0x3C) == 0);    // half a message
    midi_in_parser_reset(&p);
    CHECK(FEED(&p, e, 0x40, 0x64) == 0);    // its data bytes do not complete a note
    CHECK(FEED(&p, e, 0x90, 0x40, 0x64) == 1);
}

int main(void) {
    test_note_on_and_off_on_every_channel();
    test_note_on_velocity_zero_is_note_off();
    test_running_status();
    test_realtime_bytes_change_nothing();
    test_system_reset_resets_the_parser();
    test_other_channel_messages_produce_nothing();
    test_program_change_keeps_its_running_status();
    test_all_sound_off_and_all_notes_off();
    test_sysex_is_skipped_and_ends_running_status();
    test_system_common_drops_a_note_in_progress();
    test_data_bytes_without_a_status_are_ignored();
    test_a_new_status_drops_the_partial_message();
    test_reset_ignores_data_until_a_status();
    if (failures) { printf("%d check(s) failed\n", failures); return 1; }
    printf("midi_in tests passed\n");
    return 0;
}

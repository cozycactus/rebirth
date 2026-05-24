cmake_minimum_required(VERSION 3.13)
project(rebirth_tests C)

set(CMAKE_C_STANDARD 11)

set(REBIRTH_SRC ${CMAKE_SOURCE_DIR}/../src)
set(REBIRTH_INC ${CMAKE_SOURCE_DIR}/../include)
set(REBIRTH_HOST_DEFS REBIRTH_HOST_TEST=1)

# Test 1: pattern logic (pure, no hardware deps)
add_executable(test_pattern
    test_pattern.c
    ${REBIRTH_SRC}/pattern.c
)
target_include_directories(test_pattern PRIVATE ${REBIRTH_INC})
target_compile_definitions(test_pattern PRIVATE ${REBIRTH_HOST_DEFS})

# Test 2: sync layer (MIDI buffer + gate commit, mocked hardware)
add_executable(test_sync
    test_sync.c
    ${REBIRTH_SRC}/midi_out.c
    ${REBIRTH_SRC}/cv_gate.c
)
target_include_directories(test_sync PRIVATE ${REBIRTH_INC})
target_compile_definitions(test_sync PRIVATE ${REBIRTH_HOST_DEFS} MOCK_HARDWARE=1)

# Test 3: song mode (song editing, pattern queue, mutes, A/B)
add_executable(test_song
    test_song.c
    ${REBIRTH_SRC}/song.c
    ${REBIRTH_SRC}/pattern.c
    ${REBIRTH_SRC}/shared_state.c
)
target_include_directories(test_song PRIVATE ${REBIRTH_INC})
target_compile_definitions(test_song PRIVATE ${REBIRTH_HOST_DEFS})

enable_testing()
add_test(NAME pattern_tests COMMAND test_pattern)
add_test(NAME sync_tests COMMAND test_sync)
add_test(NAME song_tests COMMAND test_song)

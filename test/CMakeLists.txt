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

add_executable(test_kit
    test_kit.c
    ${REBIRTH_SRC}/kit.c
    ${REBIRTH_SRC}/pattern.c
)
target_include_directories(test_kit PRIVATE ${REBIRTH_INC})
target_compile_definitions(test_kit PRIVATE ${REBIRTH_HOST_DEFS})
add_test(NAME kit_tests COMMAND test_kit)

# Test 2: sync layer (MIDI buffer + gate commit, mocked hardware)
add_executable(test_sync
    test_sync.c
    ${REBIRTH_SRC}/midi_out.c
    ${REBIRTH_SRC}/cv_gate.c
)
target_include_directories(test_sync PRIVATE ${REBIRTH_INC})
target_compile_definitions(test_sync PRIVATE ${REBIRTH_HOST_DEFS} MOCK_HARDWARE=1)

# Test 3: song mode (song editing, pattern queue, mutes, banks)
add_executable(test_song
    test_song.c
    fakes/fake_hardware.c
    ${REBIRTH_SRC}/song.c
    ${REBIRTH_SRC}/pattern.c
    ${REBIRTH_SRC}/kit.c
    ${REBIRTH_SRC}/shared_state.c
    ${REBIRTH_SRC}/sequencer.c
    ${REBIRTH_SRC}/midi_out.c
)
target_include_directories(test_song PRIVATE ${REBIRTH_INC} ${CMAKE_SOURCE_DIR}/fakes)
target_compile_definitions(test_song PRIVATE ${REBIRTH_HOST_DEFS})

enable_testing()
add_executable(test_playback
    test_playback.c
    fakes/fake_hardware.c
    ${REBIRTH_SRC}/song.c
    ${REBIRTH_SRC}/pattern.c
    ${REBIRTH_SRC}/kit.c
    ${REBIRTH_SRC}/shared_state.c
    ${REBIRTH_SRC}/midi_out.c
)
target_include_directories(test_playback PRIVATE ${REBIRTH_INC} ${CMAKE_SOURCE_DIR}/fakes)
target_compile_definitions(test_playback PRIVATE ${REBIRTH_HOST_DEFS})
if(UNIX)
    find_package(Threads REQUIRED)
    target_compile_definitions(test_playback PRIVATE REBIRTH_THREADED_TEST=1)
    target_link_libraries(test_playback PRIVATE Threads::Threads)
endif()
add_test(NAME playback_tests COMMAND test_playback)
set_tests_properties(playback_tests PROPERTIES TIMEOUT 30)
add_test(NAME pattern_tests COMMAND test_pattern)
add_test(NAME sync_tests COMMAND test_sync)
add_executable(test_sync_gain2 test_sync.c ${REBIRTH_SRC}/midi_out.c ${REBIRTH_SRC}/cv_gate.c)
target_include_directories(test_sync_gain2 PRIVATE ${REBIRTH_INC})
target_compile_definitions(test_sync_gain2 PRIVATE MOCK_HARDWARE=1 CV_DAC_GAIN=2)
add_test(NAME sync_gain2_tests COMMAND test_sync_gain2)
add_test(NAME song_tests COMMAND test_song)

add_executable(test_storage
    test_storage.c
    fakes/fake_hardware.c
    ${REBIRTH_SRC}/sequencer.c
    ${REBIRTH_SRC}/pattern.c
    ${REBIRTH_SRC}/kit.c
    ${REBIRTH_SRC}/song.c
    ${REBIRTH_SRC}/shared_state.c
    ${REBIRTH_SRC}/midi_out.c
)
target_include_directories(test_storage PRIVATE ${REBIRTH_INC} ${CMAKE_SOURCE_DIR}/fakes)
add_test(NAME storage_tests COMMAND test_storage)

add_executable(test_ui
    test_ui.c
    fakes/fake_hardware.c
    ${REBIRTH_SRC}/pattern.c
    ${REBIRTH_SRC}/kit.c
    ${REBIRTH_SRC}/song.c
    ${REBIRTH_SRC}/shared_state.c
    ${REBIRTH_SRC}/midi_out.c
    ${REBIRTH_SRC}/flash_storage.c
    ${REBIRTH_SRC}/display.c
    ${REBIRTH_SRC}/ssd1309.c
    ${REBIRTH_SRC}/session.c
    ${REBIRTH_SRC}/led_frame.c
)
target_include_directories(test_ui PRIVATE ${REBIRTH_INC} ${CMAKE_SOURCE_DIR}/fakes)
add_test(NAME ui_tests COMMAND test_ui)

add_executable(test_session
    test_session.c
    fakes/fake_hardware.c
    ${REBIRTH_SRC}/session.c
    ${REBIRTH_SRC}/sequencer.c
    ${REBIRTH_SRC}/pattern.c
    ${REBIRTH_SRC}/kit.c
    ${REBIRTH_SRC}/song.c
    ${REBIRTH_SRC}/shared_state.c
    ${REBIRTH_SRC}/midi_out.c
)
target_include_directories(test_session PRIVATE ${REBIRTH_INC} ${CMAKE_SOURCE_DIR}/fakes)
add_test(NAME session_tests COMMAND test_session)

add_executable(test_led_frame
    test_led_frame.c
    ${REBIRTH_SRC}/led_frame.c
)
target_include_directories(test_led_frame PRIVATE ${REBIRTH_INC})
add_test(NAME led_frame_tests COMMAND test_led_frame)

add_executable(test_panel_input
    test_panel_input.c
    fakes/fake_hardware.c
    ${REBIRTH_SRC}/tca8418.c
    ${REBIRTH_SRC}/panel_input.c
)
target_include_directories(test_panel_input PRIVATE ${REBIRTH_INC} ${CMAKE_SOURCE_DIR}/fakes)
add_test(NAME panel_input_tests COMMAND test_panel_input)

add_executable(test_expanders
    test_expanders.c
    fakes/fake_hardware.c
    ${REBIRTH_SRC}/mcp23017.c
    ${REBIRTH_SRC}/step_leds.c
    ${REBIRTH_SRC}/encoder.c
)
target_include_directories(test_expanders PRIVATE ${REBIRTH_INC} ${CMAKE_SOURCE_DIR}/fakes)
add_test(NAME expanders_tests COMMAND test_expanders)

add_executable(test_encoder
    test_encoder.c
    fakes/fake_hardware.c
    ${REBIRTH_SRC}/encoder.c
)
target_include_directories(test_encoder PRIVATE ${REBIRTH_INC} ${CMAKE_SOURCE_DIR}/fakes)
add_test(NAME encoder_tests COMMAND test_encoder)

add_executable(test_midi_in
    test_midi_in.c
    ${REBIRTH_SRC}/midi_in.c
)
target_include_directories(test_midi_in PRIVATE ${REBIRTH_INC})
target_compile_definitions(test_midi_in PRIVATE ${REBIRTH_HOST_DEFS})
add_test(NAME midi_in_tests COMMAND test_midi_in)

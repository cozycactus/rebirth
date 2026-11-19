# == DO NOT EDIT THE FOLLOWING LINES for the Raspberry Pi Pico VS Code Extension to work ==
if(WIN32)
    set(USERHOME $ENV{USERPROFILE})
else()
    set(USERHOME $ENV{HOME})
endif()
set(sdkVersion 2.2.0)
set(toolchainVersion 14_2_Rel1)
set(picotoolVersion 2.2.0-a4)
set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
if (EXISTS ${picoVscode})
    include(${picoVscode})
endif()
# ====================================================================================
set(PICO_BOARD pico CACHE STRING "Board type")

cmake_minimum_required(VERSION 3.13)

# Pull in Pico SDK (must set PICO_SDK_PATH env var or cmake var)
include(pico_sdk_import.cmake)

project(rp2040_sequencer C CXX ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

add_executable(rp2040_sequencer
    src/main.c
    src/sequencer.c
    src/pattern.c
    src/kit.c
    src/midi_out.c
    src/midi_in.c
    src/clock_in.c
    src/midi_song.c
    src/cv_gate.c
    src/song.c
    src/session.c
    src/led_frame.c
    src/rgb_leds.c
    src/display.c
    src/ui.c
    src/ssd1309.c
    src/encoder.c
    src/mcp23017.c
    src/key_scan.c
    src/panel_input.c
    src/step_leds.c
    src/flash_storage.c
    src/shared_state.c
)

target_include_directories(rp2040_sequencer PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

set(REBIRTH_CV_DAC_GAIN "1" CACHE STRING "MCP4822 gain: 1 (2.048 V) or 2 (4.096 V)")
if(NOT REBIRTH_CV_DAC_GAIN MATCHES "^[12]$")
    message(FATAL_ERROR "REBIRTH_CV_DAC_GAIN must be 1 or 2")
endif()
target_compile_definitions(rp2040_sequencer PRIVATE CV_DAC_GAIN=${REBIRTH_CV_DAC_GAIN})
target_link_options(rp2040_sequencer PRIVATE "LINKER:-T,${CMAKE_CURRENT_SOURCE_DIR}/midi_song_flash.ld")
set_property(TARGET rp2040_sequencer APPEND PROPERTY LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/midi_song_flash.ld")

target_link_libraries(rp2040_sequencer
    pico_stdlib
    pico_unique_id
    pico_multicore
    hardware_spi
    hardware_uart
    hardware_timer
    hardware_sync
    hardware_i2c        # For MCP23017 button expander
    hardware_gpio
    hardware_flash
    hardware_pio
    hardware_dma
)

pico_generate_pio_header(rp2040_sequencer ${CMAKE_CURRENT_LIST_DIR}/src/ws2812.pio)

# UART0 is used for DIN-5 MIDI, so UART stdio is off. No USB stdio needed.
pico_enable_stdio_usb(rp2040_sequencer 0)
pico_enable_stdio_uart(rp2040_sequencer 0)

# Generate UF2 for drag-and-drop flashing. A Pico 2 image gets its own name:
# the boot ROM of one chip refuses the UF2 of the other.
if(PICO_BOARD STREQUAL "pico2")
    set_target_properties(rp2040_sequencer PROPERTIES OUTPUT_NAME rebirth_pico2)
    pico_set_program_name(rp2040_sequencer "ReBirth Pico 2")
endif()
pico_add_extra_outputs(rp2040_sequencer)

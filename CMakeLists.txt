cmake_minimum_required(VERSION 3.13)

# Pull in Pico SDK (must set PICO_SDK_PATH env var)
include($ENV{PICO_SDK_PATH}/external/pico_sdk_import.cmake)

project(rp2040_sequencer C CXX ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

add_executable(rp2040_sequencer
    src/main.c
    src/sequencer.c
    src/pattern.c
    src/midi_out.c
    src/cv_gate.c
    src/song.c
    src/display.c
    src/ui.c
)

target_include_directories(rp2040_sequencer PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(rp2040_sequencer
    pico_stdlib
    pico_multicore
    hardware_spi
    hardware_uart
    hardware_timer
    hardware_sync
    hardware_i2c        # For MCP23017 button expander
    hardware_gpio
    # tinyusb_device    # Uncomment for USB MIDI
    # tinyusb_board
)

# Enable USB output for debug, disable UART stdout (UART0 is MIDI)
pico_enable_stdio_usb(rp2040_sequencer 1)
pico_enable_stdio_uart(rp2040_sequencer 0)

# Generate UF2 for drag-and-drop flashing
pico_add_extra_outputs(rp2040_sequencer)

cmake_minimum_required(VERSION 3.13)

# Initialize the Pico SDK
include($ENV{PICO_SDK_PATH}/external/pico_sdk_import.cmake)

project(hecate C CXX ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

# Initialize the Raspberry Pi Pico SDK
pico_sdk_init()

# Add Pico-PIO-USB
add_subdirectory(pico-pio-usb)

# Add executable
add_executable(hecate
    src/main.c
    src/ps2out.c
    src/ps2_keyboard.c
    src/ps2_mouse.c
)

# Generate PIO headers
pico_generate_pio_header(hecate ${CMAKE_CURRENT_LIST_DIR}/src/ps2out.pio)

# Include directories
target_include_directories(hecate PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/src
)

# Link libraries
target_link_libraries(hecate
    pico_stdlib
    pico_multicore
    hardware_pio
    tinyusb_host
    tinyusb_board
    pico_pio_usb
)

# Disable USB serial (conflicts with TinyUSB host), enable UART
pico_enable_stdio_usb(hecate 0)
pico_enable_stdio_uart(hecate 1)

# Create map/bin/hex/uf2 files
pico_add_extra_outputs(hecate)

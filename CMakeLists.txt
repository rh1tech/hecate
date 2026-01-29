cmake_minimum_required(VERSION 3.13)

# Initialize the Pico SDK
include($ENV{PICO_SDK_PATH}/external/pico_sdk_import.cmake)

project(hecate C CXX ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

# Option for RP2040-Zero with WS2812 RGB LED
option(USE_WS2812 "Use WS2812 RGB LED (RP2040-Zero)" OFF)

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
    src/led.c
    src/hcd_hybrid.c
)

# Generate PIO headers
pico_generate_pio_header(hecate ${CMAKE_CURRENT_LIST_DIR}/src/ps2out.pio)

# Include directories
target_include_directories(hecate PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/src
    # TinyUSB portable headers for rp2040_usb.h
    ${PICO_SDK_PATH}/lib/tinyusb/src/portable/raspberrypi/rp2040
)

# Compile definitions
target_compile_definitions(hecate PRIVATE
    CFG_TUH_RPI_HYBRID_USB=1
)

if(USE_WS2812)
    target_compile_definitions(hecate PRIVATE USE_WS2812=1)
    message(STATUS "Building with WS2812 RGB LED support (RP2040-Zero)")
else()
    message(STATUS "Building with standard GPIO LED (Raspberry Pi Pico)")
endif()

# For hybrid mode, we need to build TinyUSB without the conflicting HCD files
# We'll use tinyusb_host but wrap the conflicting symbols

# Link libraries - order matters!
target_link_libraries(hecate
    pico_stdlib
    pico_multicore
    hardware_pio
    hardware_resets
    tinyusb_host
    tinyusb_board
    pico_pio_usb
)

# Allow multiple definitions - our hcd_hybrid.c will be linked first
# and the SDK's HCD files will be ignored
target_link_options(hecate PRIVATE
    -Wl,--allow-multiple-definition
)

# Enable UART serial console (GPIO 0=TX, GPIO 1=RX)
# Note: USB serial not possible when tinyusb_host is linked (SDK limitation)
pico_enable_stdio_usb(hecate 0)
pico_enable_stdio_uart(hecate 1)

# Create map/bin/hex/uf2 files
pico_add_extra_outputs(hecate)

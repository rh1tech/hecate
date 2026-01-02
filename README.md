# Hecate

USB to PS/2 Keyboard and Mouse Converter for Raspberry Pi Pico

## Overview

Hecate is a firmware for the Raspberry Pi Pico (RP2040) that converts USB keyboards and mice to PS/2 protocol. It allows you to use modern USB peripherals with vintage computers and systems that only support PS/2 input devices.

## Features

### USB Host
- **Dual PIO-USB ports** - Connect keyboard and mouse directly without a hub
- **USB hub support** - Or use a hub for multiple devices on one port
- **HID report parsing** - Supports both boot protocol and full HID report descriptors
- **NKRO support** - N-Key Rollover for gaming keyboards

### PS/2 Keyboard Emulation
- **Full Scancode Set 2** - Complete key mapping including all standard keys
- **Extended keys** - Navigation, multimedia, and special keys with E0 prefix
- **Key repeat (typematic)** - Configurable repeat rate and delay
- **LED feedback** - Caps Lock, Num Lock, Scroll Lock sync with host
- **Host commands** - Reset, Echo, Identify, Set LEDs, Set Typematic Rate
- **Special sequences** - Proper Pause/Break and Print Screen handling

### PS/2 Mouse Emulation
- **Standard 3-button mouse** - Left, right, middle buttons
- **IntelliMouse** - Scroll wheel support (auto-detected)
- **IntelliMouse Explorer** - 5-button support (auto-detected)
- **Host commands** - Reset, Get ID, Enable/Disable streaming, Status request

## Pin Configuration

| Function | GPIO | Description |
|----------|------|-------------|
| USB0 D+ | GPIO 2 | USB Port 0 Data+ |
| USB0 D- | GPIO 3 | USB Port 0 Data- |
| USB1 D+ | GPIO 4 | USB Port 1 Data+ |
| USB1 D- | GPIO 5 | USB Port 1 Data- |
| PS/2 KB DATA | GPIO 11 | Keyboard Data line |
| PS/2 KB CLK | GPIO 12 | Keyboard Clock line |
| PS/2 MS DATA | GPIO 14 | Mouse Data line |
| PS/2 MS CLK | GPIO 15 | Mouse Clock line |

## Building

### Prerequisites

- [Pico SDK](https://github.com/raspberrypi/pico-sdk) installed
- `PICO_SDK_PATH` environment variable set
- ARM GCC toolchain

### Build Commands

```bash
mkdir build
cd build
cmake ..
make -j4
```

The firmware will be generated as `build/hecate.uf2`.

### Flashing

1. Hold the BOOTSEL button on the Pico
2. Connect to USB while holding BOOTSEL
3. Release BOOTSEL - the Pico appears as a USB drive
4. Copy `hecate.uf2` to the drive

Or use the included flash script:
```bash
./flash.sh
```

## Hardware Notes

### PS/2 Connections

PS/2 uses open-drain signaling. The DATA and CLK lines should be directly connected to the Pico GPIO pins. The Pico's internal pull-ups are not used; the PS/2 host provides the pull-ups.

```
PS/2 Connector (female, from device side):
    6 5
   4   3
    2 1

Pin 1: DATA
Pin 3: GND
Pin 4: +5V
Pin 5: CLK
Pin 6: N/C
```

### USB Connections

Each USB port requires D+ and D- connections. The D- pin is always D+ pin + 1.

For a USB Type-A connector:
- Pin 1: +5V (VBUS)
- Pin 2: D-
- Pin 3: D+
- Pin 4: GND

## License

MIT License

Copyright (c) 2025 Mikhail Matveev <xtreme@rh1.tech>

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

## Acknowledgments

This project incorporates code and concepts from:

- **[ps2x2pico](https://github.com/No0ne/ps2x2pico)** by No0ne - PS/2 protocol implementation and PIO programs
  - Copyright (c) 2024-2025 No0ne
  - Copyright (c) 2023 Dustin Hoffman
  - Licensed under MIT License

- **[Pico-PIO-USB](https://github.com/sekigon-gonnoc/Pico-PIO-USB)** by sekigon-gonnoc - USB host implementation using PIO
  - Licensed under MIT License

- **[TinyUSB](https://github.com/hathach/tinyusb)** by Ha Thach - USB stack
  - Licensed under MIT License

- **[Raspberry Pi Pico SDK](https://github.com/raspberrypi/pico-sdk)** - Hardware abstraction layer
  - Licensed under BSD 3-Clause License

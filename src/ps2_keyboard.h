/*
 * Hecate - PS/2 Keyboard Emulation Driver
 *
 * Public interface for the PS/2 keyboard emulation.
 * Provides functions to send key events and manage keyboard state.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef PS2_KEYBOARD_H
#define PS2_KEYBOARD_H

#include "ps2out.h"

// Keyboard GPIO configuration (CLK = DATA + 1)
#define PS2_KB_DATA_PIN  11
#define PS2_KB_CLK_PIN   12

// Initialize PS/2 keyboard emulation
void ps2_keyboard_init(void);

// Send a key event (handles make/break codes)
void ps2_keyboard_send_key(u8 hid_key, bool pressed);

// Set keyboard LEDs (called from USB callback)
void ps2_keyboard_set_leds(u8 leds);

// Get current LED callback for USB LED sync
s64 ps2_keyboard_led_callback(void);

// Process keyboard tasks (call in main loop)
bool ps2_keyboard_task(void);

#endif // PS2_KEYBOARD_H

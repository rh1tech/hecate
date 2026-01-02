/*
 * Hecate - PS/2 Mouse Emulation Driver
 *
 * Public interface for the PS/2 mouse emulation.
 * Provides functions to send mouse movement and button events.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef PS2_MOUSE_H
#define PS2_MOUSE_H

#include "ps2out.h"

// Mouse GPIO configuration (CLK = DATA + 1)
#define PS2_MOUSE_DATA_PIN  14
#define PS2_MOUSE_CLK_PIN   15

// Initialize PS/2 mouse emulation
void ps2_mouse_init(void);

// Send mouse movement (called from USB HID callback)
// buttons: bit0=left, bit1=right, bit2=middle, bit3=back, bit4=forward
void ps2_mouse_send_movement(u8 buttons, s8 x, s8 y, s8 wheel);

// Process mouse tasks (call in main loop)
bool ps2_mouse_task(void);

#endif // PS2_MOUSE_H

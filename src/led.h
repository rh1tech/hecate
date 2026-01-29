/*
 * Hecate - LED Driver
 *
 * Supports both RP2040-Zero (WS2812 RGB LED on GPIO16) and
 * Raspberry Pi Pico (simple LED on GPIO25).
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef LED_H
#define LED_H

#include <stdbool.h>

// Initialize LED (auto-detects board type)
void led_init(void);

// Set connection state (LED on when connected)
void led_set_connected(bool keyboard, bool mouse);

// Trigger activity blink (keypress or mouse button) - blue
void led_blink_activity(void);

// Call periodically from main loop
void led_task(void);

#endif // LED_H

#ifndef USB_KEYBOARD_H
#define USB_KEYBOARD_H

#include <stdint.h>
#include <stdbool.h>

// USB Host configuration
#define USB1_DP_PIN 2   // D1+ on GPIO 2, D1- on GPIO 3
#define USB2_DP_PIN 4   // D2+ on GPIO 4, D2- on GPIO 5

// Initialize USB PIO host
void usb_keyboard_init(void);

// Process USB keyboard events (call in main loop)
void usb_keyboard_task(void);

// Callback when a key event occurs (implemented in main.c)
extern void on_keyboard_event(uint8_t modifiers, uint8_t keycode, bool pressed);

#endif // USB_KEYBOARD_H

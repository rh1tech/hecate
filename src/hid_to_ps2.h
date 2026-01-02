#ifndef HID_TO_PS2_H
#define HID_TO_PS2_H

#include <stdint.h>
#include <stdbool.h>

// Convert USB HID keycode to PS/2 scancode
// Returns the PS/2 scancode, or 0 if no mapping exists
// Sets *extended to true if E0 prefix is needed
uint8_t hid_to_ps2(uint8_t hid_keycode, bool *extended);

// Send a key event to PS/2 (handles make/break codes)
void send_ps2_key(uint8_t hid_keycode, bool pressed);

#endif // HID_TO_PS2_H

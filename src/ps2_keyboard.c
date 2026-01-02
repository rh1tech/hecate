/*
 * Hecate - PS/2 Keyboard Emulation Driver
 *
 * This driver emulates a PS/2 keyboard, translating USB HID keycodes
 * to PS/2 Scancode Set 2 and handling bidirectional communication
 * with the PS/2 host.
 *
 * Features:
 *   - Full Scancode Set 2 support
 *   - Key repeat (typematic) with configurable rate and delay
 *   - LED feedback (Caps Lock, Num Lock, Scroll Lock)
 *   - Host command handling (Reset, Echo, Identify, Set LEDs, etc.)
 *   - Special key sequences (Pause/Break, Print Screen)
 *   - Extended key support (E0 prefix)
 *
 * SPDX-License-Identifier: MIT
 */

#include "ps2_keyboard.h"
#include "tusb.h"
#include <stdio.h>

static ps2out kb_out;

static bool kb_enabled = true;
static u8 kb_modifiers = 0;
static u8 kb_repeat_key = 0;
static u16 kb_delay_ms = 500;
static u32 kb_repeat_us = 91743;
static alarm_id_t kb_repeater = 0;

// LED state for USB keyboard sync
u8 kb_set_led = 0;

// PS/2 to LED conversion table
static const u8 led2ps2[] = { 0, 4, 1, 5, 2, 6, 3, 7 };

// HID modifier to PS/2 scancode mapping (Set 2)
static const u8 mod2ps2[] = { 0x14, 0x12, 0x11, 0x1f, 0x14, 0x59, 0x11, 0x27 };

// HID keycode to PS/2 scancode mapping (Set 2)
static const u8 hid2ps2[] = {
    0x00, 0x00, 0xfc, 0x00, 0x1c, 0x32, 0x21, 0x23, 0x24, 0x2b, 0x34, 0x33, 0x43, 0x3b, 0x42, 0x4b,
    0x3a, 0x31, 0x44, 0x4d, 0x15, 0x2d, 0x1b, 0x2c, 0x3c, 0x2a, 0x1d, 0x22, 0x35, 0x1a, 0x16, 0x1e,
    0x26, 0x25, 0x2e, 0x36, 0x3d, 0x3e, 0x46, 0x45, 0x5a, 0x76, 0x66, 0x0d, 0x29, 0x4e, 0x55, 0x54,
    0x5b, 0x5d, 0x5d, 0x4c, 0x52, 0x0e, 0x41, 0x49, 0x4a, 0x58, 0x05, 0x06, 0x04, 0x0c, 0x03, 0x0b,
    0x83, 0x0a, 0x01, 0x09, 0x78, 0x07, 0x7c, 0x7e, 0x7e, 0x70, 0x6c, 0x7d, 0x71, 0x69, 0x7a, 0x74,
    0x6b, 0x72, 0x75, 0x77, 0x4a, 0x7c, 0x7b, 0x79, 0x5a, 0x69, 0x72, 0x7a, 0x6b, 0x73, 0x74, 0x6c,
    0x75, 0x7d, 0x70, 0x71, 0x61, 0x2f, 0x37, 0x0f, 0x08, 0x10, 0x18, 0x20, 0x28, 0x30, 0x38, 0x40,
    0x48, 0x50, 0x57, 0x5f
};

// Typematic repeat rates (microseconds between repeats)
static const u32 kb_repeats[] = {
    33333, 37453, 41667, 45872, 48309, 54054, 58480, 62500,
    66667, 75188, 83333, 91743, 100000, 108696, 116279, 125000,
    133333, 149254, 166667, 181818, 200000, 217391, 232558, 250000,
    270270, 303030, 333333, 370370, 400000, 434783, 476190, 500000
};

// Typematic delays (milliseconds before first repeat)
static const u16 kb_delays[] = { 250, 500, 750, 1000 };

static s64 kb_led_callback(alarm_id_t id, void *user_data);

static void kb_set_leds_internal(u8 byte) {
    if (byte > 7) byte = 0;
    kb_set_led = led2ps2[byte];
    add_alarm_in_us(1000, kb_led_callback, NULL, false);
}

static s64 kb_reset_callback(alarm_id_t id, void *user_data) {
    (void)id;
    (void)user_data;
    kb_set_leds_internal(0);
    kb_out.packet[1] = 0xaa;
    ps2out_send(&kb_out, 1);
    kb_enabled = true;
    return 0;
}

static bool key_is_modifier(u8 key) {
    return key >= HID_KEY_CONTROL_LEFT && key <= HID_KEY_GUI_RIGHT;
}

static bool key_is_extended(u8 key) {
    return key == HID_KEY_PRINT_SCREEN ||
          (key >= HID_KEY_INSERT && key <= HID_KEY_ARROW_UP) ||
           key == HID_KEY_KEYPAD_DIVIDE ||
           key == HID_KEY_KEYPAD_ENTER ||
           key == HID_KEY_APPLICATION ||
           key == HID_KEY_POWER ||
          (key >= HID_KEY_GUI_LEFT && key != HID_KEY_SHIFT_RIGHT);
}

static s64 kb_repeat_callback(alarm_id_t id, void *user_data) {
    (void)id;
    (void)user_data;
    
    if (kb_repeat_key) {
        if (kb_enabled) {
            u8 len = 0;
            if (key_is_extended(kb_repeat_key)) kb_out.packet[++len] = 0xe0;

            if (key_is_modifier(kb_repeat_key)) {
                kb_out.packet[++len] = mod2ps2[kb_repeat_key - HID_KEY_CONTROL_LEFT];
            } else {
                kb_out.packet[++len] = hid2ps2[kb_repeat_key];
            }

            ps2out_send(&kb_out, len);
        }

        return kb_repeat_us;
    }

    kb_repeater = 0;
    return 0;
}

static void kb_receive(u8 byte, u8 prev_byte) {
    printf("KB RX: 0x%02x (prev: 0x%02x)\n", byte, prev_byte);
    
    switch (prev_byte) {
        case 0xed: // Set LEDs
            kb_set_leds_internal(byte);
            break;

        case 0xf0: // Set scan code set (we only support set 2)
            // Just acknowledge, we always use set 2
            break;

        case 0xf3: // Set typematic rate and delay
            kb_repeat_us = kb_repeats[byte & 0x1f];
            kb_delay_ms = kb_delays[(byte & 0x60) >> 5];
            break;

        default:
            switch (byte) {
                case 0xff: // Reset
                    printf("KB: Reset received, disabling\n");
                    kb_enabled = false;
                    kb_repeat_us = 91743;
                    kb_delay_ms = 500;
                    kb_set_leds_internal(7); // All LEDs on during reset
                    add_alarm_in_ms(500, kb_reset_callback, NULL, false);
                    break;

                case 0xee: // Echo
                    kb_out.packet[1] = 0xee;
                    ps2out_send(&kb_out, 1);
                    return;

                case 0xf0: // Get/Set scan code set
                case 0xf3: // Set typematic rate/delay
                case 0xed: // Set LEDs - wait for parameter byte
                    // These commands expect a parameter byte, just ACK and wait
                    break;

                case 0xf2: // Identify keyboard
                    kb_out.packet[1] = 0xfa;
                    kb_out.packet[2] = 0xab;
                    kb_out.packet[3] = 0x83;
                    ps2out_send(&kb_out, 3);
                    return;

                case 0xf4: // Enable scanning
                    printf("KB: Scanning enabled\n");
                    kb_enabled = true;
                    break;

                case 0xf5: // Disable scanning, restore default parameters
                    printf("KB: Disabled (0xF5)\n");
                    kb_enabled = false;
                    kb_repeat_us = 91743;
                    kb_delay_ms = 500;
                    kb_set_leds_internal(0);
                    break;

                case 0xf6: // Set default parameters (keep scanning enabled)
                    printf("KB: Set defaults (0xF6)\n");
                    kb_repeat_us = 91743;
                    kb_delay_ms = 500;
                    kb_set_leds_internal(0);
                    // Note: F6 does NOT disable scanning
                    break;

                default:
                    printf("KB: Unknown command 0x%02x\n", byte);
                    break;
            }
            break;
    }

    // Send ACK
    kb_out.packet[1] = 0xfa;
    ps2out_send(&kb_out, 1);
}

void ps2_keyboard_send_key(u8 key, bool state) {
    // Handle modifiers
    if (key >= HID_KEY_CONTROL_LEFT && key <= HID_KEY_GUI_RIGHT) {
        if (state) {
            kb_modifiers = kb_modifiers | (1 << (key - HID_KEY_CONTROL_LEFT));
        } else {
            kb_modifiers = kb_modifiers & ~(1 << (key - HID_KEY_CONTROL_LEFT));
        }
    } else if (key < HID_KEY_A || key > HID_KEY_F24) {
        return;
    }

    u8 len = 0;

    if (!kb_enabled) {
        printf("KB: disabled\n");
        return;
    }

    // Special handling for Pause key
    if (key == HID_KEY_PAUSE) {
        kb_repeat_key = 0;

        if (state) {
            if (kb_modifiers & KEYBOARD_MODIFIER_LEFTCTRL ||
                kb_modifiers & KEYBOARD_MODIFIER_RIGHTCTRL) {
                // Ctrl+Pause = Break
                kb_out.packet[++len] = 0xe0;
                kb_out.packet[++len] = 0x7e;
                kb_out.packet[++len] = 0xe0;
                kb_out.packet[++len] = 0xf0;
                kb_out.packet[++len] = 0x7e;
            } else {
                // Pause sequence
                kb_out.packet[++len] = 0xe1;
                kb_out.packet[++len] = 0x14;
                kb_out.packet[++len] = 0x77;
                kb_out.packet[++len] = 0xe1;
                kb_out.packet[++len] = 0xf0;
                kb_out.packet[++len] = 0x14;
                kb_out.packet[++len] = 0xf0;
                kb_out.packet[++len] = 0x77;
            }

            ps2out_send(&kb_out, len);
        }

        return;
    }

    // Add E0 prefix for extended keys
    if (key_is_extended(key)) kb_out.packet[++len] = 0xe0;

    if (state) {
        // Key press - set up repeat
        kb_repeat_key = key;
        if (kb_repeater) cancel_alarm(kb_repeater);
        kb_repeater = add_alarm_in_ms(kb_delay_ms, kb_repeat_callback, NULL, false);
    } else {
        // Key release
        if (key == kb_repeat_key) kb_repeat_key = 0;
        kb_out.packet[++len] = 0xf0;
    }

    // Add scancode
    if (key >= HID_KEY_CONTROL_LEFT && key <= HID_KEY_GUI_RIGHT) {
        kb_out.packet[++len] = mod2ps2[key - HID_KEY_CONTROL_LEFT];
    } else {
        kb_out.packet[++len] = hid2ps2[key];
    }

    ps2out_send(&kb_out, len);
}

void ps2_keyboard_set_leds(u8 leds) {
    kb_set_led = leds;
}

static s64 kb_led_callback(alarm_id_t id, void *user_data) {
    (void)id;
    (void)user_data;
    // This callback is for internal LED timing
    return 0;
}

s64 ps2_keyboard_led_callback(void) {
    // This would be called to sync LEDs with USB keyboards
    // Implementation depends on how USB LED output reports are handled
    return 0;
}

bool ps2_keyboard_task(void) {
    ps2out_task(&kb_out);
    return kb_enabled && !ps2out_is_busy();
}

void ps2_keyboard_init(void) {
    // Use state machines 0 (TX) and 1 (RX) for keyboard
    ps2out_init(&kb_out, 0, PS2_KB_DATA_PIN, &kb_receive);
    
    // Send self-test passed
    add_alarm_in_ms(500, kb_reset_callback, NULL, false);
    
    printf("PS/2 Keyboard initialized: DATA=GPIO%d, CLK=GPIO%d\n", 
           PS2_KB_DATA_PIN, PS2_KB_CLK_PIN);
}

/*
 * Hecate - PS/2 Mouse Emulation Driver
 *
 * This driver emulates a PS/2 mouse, translating USB HID mouse reports
 * to the PS/2 mouse protocol with full bidirectional communication.
 *
 * Features:
 *   - Standard 3-button PS/2 mouse protocol
 *   - IntelliMouse extensions (scroll wheel)
 *   - IntelliMouse Explorer (5-button + wheel)
 *   - Automatic protocol detection via magic sequence
 *   - Configurable sample rate (host-controlled)
 *   - Movement accumulation and overflow handling
 *
 * SPDX-License-Identifier: MIT
 */

#include "ps2_mouse.h"
#include <stdio.h>

static ps2out ms_out;

#define MS_RATE_DEFAULT 100

static bool ms_streaming = false;
static bool ms_ismoving = false;
static u32 ms_magic_seq = 0;
static u8 ms_type = 0;      // 0=standard, 3=IntelliMouse, 4=IntelliMouse Explorer
static u8 ms_rate = MS_RATE_DEFAULT;
static u8 ms_db = 0;        // button state
static s16 ms_dx = 0;       // accumulated X movement
static s16 ms_dy = 0;       // accumulated Y movement
static s8 ms_dz = 0;        // accumulated wheel movement

static void ms_reset(void) {
    ms_ismoving = false;
    ms_db = 0;
    ms_dx = 0;
    ms_dy = 0;
    ms_dz = 0;
}

static s64 ms_reset_callback(alarm_id_t id, void *user_data) {
    (void)id;
    (void)user_data;
    ms_out.packet[1] = 0xaa;
    ms_out.packet[2] = ms_type;
    ps2out_send(&ms_out, 2);
    return 0;
}

static u8 ms_clamp_xyz(s16 xyz) {
    if (xyz < -255) return 1;
    if (xyz > 255) return 255;
    return xyz;
}

static s16 ms_remain_xyz(s16 xyz) {
    if (xyz < -255) return xyz + 255;
    if (xyz > 255) return xyz - 255;
    return 0;
}

static s64 ms_send_callback(alarm_id_t id, void *user_data) {
    (void)id;
    (void)user_data;
    
    if (!ms_streaming) return 0;
    if (ps2out_is_busy()) return 1000000 / ms_rate;

    if (!ms_db && !ms_dx && !ms_dy && !ms_dz) {
        if (!ms_ismoving) return 1000000 / ms_rate;
        ms_ismoving = false;
    } else {
        ms_ismoving = true;
    }

    u8 byte1 = 0x08 | (ms_db & 0x07);
    u8 byte2 = ms_clamp_xyz(ms_dx);
    u8 byte3 = 0x100 - ms_clamp_xyz(ms_dy);
    s8 byte4 = 0x100 - ms_dz;

    if (ms_dx < 0) byte1 |= 0x10;
    if (ms_dy > 0) byte1 |= 0x20;
    if (byte2 == 0xaa) byte2 = 0xab;
    if (byte3 == 0xaa) byte3 = 0xab;

    u8 len = 0;
    ms_out.packet[++len] = byte1;
    ms_out.packet[++len] = byte2;
    ms_out.packet[++len] = byte3;

    if (ms_type == 3 || ms_type == 4) {
        if (byte4 < -8) byte4 = -8;
        if (byte4 > 7) byte4 = 7;

        if (ms_type == 4) {
            byte4 &= 0x0f;
            byte4 |= (ms_db << 1) & 0x30;
        }

        ms_out.packet[++len] = byte4;
    }

    ms_dx = ms_remain_xyz(ms_dx);
    ms_dy = ms_remain_xyz(ms_dy);
    ms_dz = 0;
    ps2out_send(&ms_out, len);

    return 1000000 / ms_rate;
}

void ps2_mouse_send_movement(u8 buttons, s8 x, s8 y, s8 wheel) {
    ms_db = buttons;
    ms_dx += x;
    ms_dy += y;
    ms_dz += wheel;
}

static void ms_receive(u8 byte, u8 prev_byte) {
    switch (prev_byte) {
        case 0xf3: // Set Sample Rate
            ms_rate = byte;

            ms_magic_seq = ((ms_magic_seq << 8) | byte) & 0xffffff;

            // IntelliMouse magic sequence detection
            if (ms_type == 0 && ms_magic_seq == 0xc86450) {
                ms_type = 3;  // IntelliMouse (3-button + wheel)
                printf("Mouse: IntelliMouse mode enabled\n");
            } else if (ms_type == 3 && ms_magic_seq == 0xc8c850) {
                ms_type = 4;  // IntelliMouse Explorer (5-button + wheel)
                printf("Mouse: IntelliMouse Explorer mode enabled\n");
            }

            ms_reset();
            break;

        default:
            switch (byte) {
                case 0xff: // Reset
                    add_alarm_in_ms(100, ms_reset_callback, NULL, false);
                    ms_type = 0;
                    // fall through
                case 0xf6: // Set Defaults
                    ms_rate = MS_RATE_DEFAULT;
                    // fall through
                case 0xf5: // Disable Data Reporting
                    ms_streaming = false;
                    ms_reset();
                    break;

                case 0xf4: // Enable Data Reporting
                    ms_streaming = true;
                    ms_reset();
                    add_alarm_in_ms(100, ms_send_callback, NULL, false);
                    printf("Mouse: Streaming enabled\n");
                    break;

                case 0xf2: // Get Device ID
                    ms_out.packet[1] = 0xfa;
                    ms_out.packet[2] = ms_type;
                    ps2out_send(&ms_out, 2);
                    ms_reset();
                    return;

                case 0xeb: // Read Data
                    ms_ismoving = true;
                    break;

                case 0xe9: // Status Request
                    ms_out.packet[1] = 0xfa;
                    ms_out.packet[2] = ms_streaming << 5;
                    ms_out.packet[3] = 0x02; // Resolution
                    ms_out.packet[4] = ms_rate;
                    ps2out_send(&ms_out, 4);
                    return;

                default:
                    ms_reset();
                    break;
            }
            break;
    }

    // Send ACK
    ms_out.packet[1] = 0xfa;
    ps2out_send(&ms_out, 1);
}

bool ps2_mouse_task(void) {
    ps2out_task(&ms_out);
    return ms_streaming && !ps2out_is_busy();
}

void ps2_mouse_init(void) {
    // Use state machines 2 (TX) and 3 (RX) for mouse
    // Keyboard uses state machines 0 and 1
    ps2out_init(&ms_out, 2, PS2_MOUSE_DATA_PIN, &ms_receive);
    ms_reset_callback(0, NULL);
    printf("PS/2 Mouse initialized: DATA=GPIO%d, CLK=GPIO%d\n", 
           PS2_MOUSE_DATA_PIN, PS2_MOUSE_CLK_PIN);
}

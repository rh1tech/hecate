/*
 * Hecate MouseFix - PS/2 Mouse Emulation Driver
 *
 * USB HID mouse reports are accumulated in the main context and emitted by
 * ps2_mouse_task().  There is deliberately no timer callback that competes
 * with the main loop for the packet buffer or movement counters.
 *
 * SPDX-License-Identifier: MIT
 */

#include "ps2_mouse.h"
#include "pico/stdlib.h"
#include <limits.h>

static ps2out ms_out;

#define MS_RATE_DEFAULT 100u
#define MS_RES_DEFAULT  2u

static bool ms_streaming;
static bool ms_remote;
static bool ms_scaling_2_1;
static bool ms_reset_pending;
static absolute_time_t ms_bat_deadline;
static u32 ms_next_sample_us;
static u32 ms_magic_seq;
static u8 ms_type;             // 0=standard, 3=IntelliMouse, 4=Explorer
static u8 ms_rate;
static u8 ms_resolution;
static u8 ms_buttons;
static u8 ms_last_sent_buttons;
static s32 ms_dx;
static s32 ms_dy;
static s32 ms_dz;

static s32 ms_sat_add(s32 a, s32 b) {
    if (b > 0 && a > INT32_MAX - b) return INT32_MAX;
    if (b < 0 && a < INT32_MIN - b) return INT32_MIN;
    return a + b;
}

static bool ms_queue(const u8 *bytes, u8 len) {
    if (len == 0 || len > sizeof(ms_out.packet) - 1) return false;
    for (u8 i = 0; i < len; ++i) ms_out.packet[i + 1] = bytes[i];
    return ps2out_send(&ms_out, len);
}

static bool ms_queue_byte(u8 byte) {
    return ms_queue(&byte, 1);
}

static void ms_clear_motion(void) {
    ms_dx = 0;
    ms_dy = 0;
    ms_dz = 0;
    ms_buttons = 0;
    ms_last_sent_buttons = 0;
}

static void ms_schedule_bat(u32 delay_ms) {
    ms_reset_pending = true;
    ms_bat_deadline = make_timeout_time_ms(delay_ms);
}

static bool ms_time_reached(absolute_time_t deadline) {
    return absolute_time_diff_us(get_absolute_time(), deadline) <= 0;
}

/*
 * PS/2 X/Y are 9-bit signed values: sign in byte 1 and the low 8 bits in
 * byte 2/3. The representable range is -256..255. Values outside that range
 * set the corresponding overflow flag and are saturated.
 */
static u8 ms_encode_axis(s32 value, bool *negative, bool *overflow,
                         s32 *consumed) {
    if (value < -256) {
        value = -256;
        *overflow = true;
    } else if (value > 255) {
        value = 255;
        *overflow = true;
    }
    *negative = value < 0;
    *consumed = value;
    return (u8)value;
}

static bool ms_build_and_queue_packet(bool remote_read) {
    u8 packet[4];
    u8 len = 3;
    u8 first = 0x08 | (ms_buttons & 0x07);
    bool x_negative = false, y_negative = false;
    bool x_overflow = false, y_overflow = false;
    s32 sent_x = 0, sent_y = 0;
    s32 sent_wheel = 0;

    // USB HID Y grows down; PS/2 Y grows up.
    s32 y = (ms_dy == INT32_MIN) ? INT32_MAX : -ms_dy;
    packet[1] = ms_encode_axis(ms_dx, &x_negative, &x_overflow, &sent_x);
    packet[2] = ms_encode_axis(y, &y_negative, &y_overflow, &sent_y);

    if (x_negative) first |= 0x10;
    if (y_negative) first |= 0x20;
    if (x_overflow) first |= 0x40;
    if (y_overflow) first |= 0x80;
    packet[0] = first;

    if (ms_type == 3 || ms_type == 4) {
        // Invert the USB wheel direction for PS/2. The IntelliMouse wheel
        // field is a signed 4-bit value (-8..7), so clamp the inverted value
        // and consume only the corresponding amount from the original delta.
        // This also handles the asymmetric -8..7 range without losing motion.
        s32 wheel;
        if (ms_dz < -7) {
            wheel = 7;
        } else if (ms_dz > 8) {
            wheel = -8;
        } else {
            wheel = -ms_dz;
        }
        sent_wheel = -wheel;
        packet[3] = (u8)wheel & 0x0f;

        if (ms_type == 4) {
            if (ms_buttons & 0x08) packet[3] |= 0x10; // back
            if (ms_buttons & 0x10) packet[3] |= 0x20; // forward
        }
        len = 4;
    }

    if (!ms_queue(packet, len)) return false;

    // Consume only what was actually queued. Preserve excess motion and wheel
    // movement for subsequent packets instead of silently dropping it.
    ms_dx -= sent_x;
    ms_dy += sent_y; // packet Y is inverted relative to the accumulated USB Y

    if (ms_type == 3 || ms_type == 4) {
        ms_dz -= sent_wheel;
    } else {
        // A standard PS/2 mouse has no wheel field. Do not replay old wheel
        // deltas if the host later negotiates an IntelliMouse ID.
        ms_dz = 0;
    }
    ms_last_sent_buttons = ms_buttons;
    (void)remote_read;
    return true;
}

static void ms_reset_defaults(bool reset_id) {
    ms_streaming = false;
    ms_remote = false;
    ms_scaling_2_1 = false;
    ms_rate = MS_RATE_DEFAULT;
    ms_resolution = MS_RES_DEFAULT;
    ms_magic_seq = 0;
    if (reset_id) ms_type = 0;
    ms_clear_motion();
}

static void ms_receive(u8 byte, u8 prev_byte) {
    // Parameter bytes are acknowledged just like command bytes.
    if (prev_byte == 0xf3) {
        ms_rate = byte < 10 ? 10 : (byte > 200 ? 200 : byte);
        ms_magic_seq = ((ms_magic_seq << 8) | byte) & 0xffffff;
        if (ms_type == 0 && ms_magic_seq == 0xc86450) {
            ms_type = 3;
        } else if (ms_type == 3 && ms_magic_seq == 0xc8c850) {
            ms_type = 4;
        }
        (void)ms_queue_byte(0xfa);
        return;
    }

    if (prev_byte == 0xe8) {
        ms_resolution = byte & 0x03;
        (void)ms_queue_byte(0xfa);
        return;
    }

    switch (byte) {
        case 0xff: // Reset: ACK now, BAT + ID after the reset delay.
            ms_reset_defaults(true);
            ms_schedule_bat(500);
            (void)ms_queue_byte(0xfa);
            return;

        case 0xf6: // Set Defaults
            ms_reset_defaults(false);
            break;

        case 0xf5: // Disable Data Reporting
            ms_streaming = false;
            ms_clear_motion();
            break;

        case 0xf4: // Enable Data Reporting
            ms_streaming = true;
            ms_remote = false;
            ms_clear_motion();
            ms_next_sample_us = time_us_32();
            break;

        case 0xf0: // Set Remote Mode
            ms_streaming = false;
            ms_remote = true;
            ms_clear_motion();
            break;

        case 0xea: // Set Stream Mode
            ms_remote = false;
            break;

        case 0xe6: // Scaling 1:1
            ms_scaling_2_1 = false;
            break;

        case 0xe7: // Scaling 2:1
            ms_scaling_2_1 = true;
            break;

        case 0xf3: // Set Sample Rate; next byte is parameter
        case 0xe8: // Set Resolution; next byte is parameter
            break;

        case 0xf2: { // Get Device ID
            u8 response[] = { 0xfa, ms_type };
            (void)ms_queue(response, sizeof(response));
            return;
        }

        case 0xeb: { // Read Data (Remote Mode)
            u8 ack = 0xfa;
            if (ms_queue_byte(ack)) {
                (void)ms_build_and_queue_packet(true);
            }
            return;
        }

        case 0xe9: { // Status Request
            u8 status = (ms_buttons & 0x07) |
                        (ms_scaling_2_1 ? 0x10 : 0) |
                        (ms_streaming ? 0x20 : 0) |
                        (ms_remote ? 0x40 : 0);
            u8 response[] = { 0xfa, status, ms_resolution, ms_rate };
            (void)ms_queue(response, sizeof(response));
            return;
        }

        default:
            // Unknown commands are still acknowledged, matching common mice.
            break;
    }

    (void)ms_queue_byte(0xfa);
}

void ps2_mouse_send_movement(u8 buttons, s8 x, s8 y, s8 wheel) {
    ms_buttons = buttons & 0x1f;
    ms_dx = ms_sat_add(ms_dx, x);
    ms_dy = ms_sat_add(ms_dy, y);
    ms_dz = ms_sat_add(ms_dz, wheel);
}

bool ps2_mouse_task(void) {
    ps2out_task(&ms_out);

    // Defer BAT transmission to the main loop; no alarm callback touches the
    // same queue or packet buffer as the USB and PS/2 paths.
    if (ms_reset_pending && ms_time_reached(ms_bat_deadline)) {
        u8 bat[] = { 0xaa, ms_type };
        if (ms_queue(bat, sizeof(bat))) ms_reset_pending = false;
    }

    if (ms_streaming && !ms_remote) {
        u32 now = time_us_32();
        u32 interval = 1000000u / (ms_rate ? ms_rate : 1u);
        bool changed = (ms_buttons != ms_last_sent_buttons);
        bool moved = (ms_dx != 0 || ms_dy != 0 ||
                      ((ms_type == 3 || ms_type == 4) && ms_dz != 0));

        if ((changed || moved) && (s32)(now - ms_next_sample_us) >= 0) {
            if (ms_build_and_queue_packet(false)) {
                ms_next_sample_us = now + interval;
            }
        }
    }

    return ms_streaming && !ps2out_is_busy(&ms_out);
}

void ps2_mouse_init(void) {
    ms_reset_pending = false;
    ms_reset_defaults(true);
    ps2out_init(&ms_out, 2, PS2_MOUSE_DATA_PIN, &ms_receive);
    ms_schedule_bat(500);
}

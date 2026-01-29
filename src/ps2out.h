/*
 * Hecate - PS/2 PIO Communication Driver
 *
 * Public interface for the PS/2 PIO communication layer.
 * Provides queue-based packet transmission and host command reception.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef PS2OUT_H
#define PS2OUT_H

#include <stdint.h>
#include <stdbool.h>
#include "pico/stdlib.h"
#include "pico/util/queue.h"
#include "hardware/pio.h"

typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;
typedef int64_t s64;
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

typedef void (*rx_callback)(u8 byte, u8 prev_byte);

typedef struct {
    u8 sm;              // Single state machine for TX and RX
    u8 data_pin;
    u8 clk_pin;
    queue_t packets;
    u8 packet[9];
    rx_callback rx_function;
    u8 last_rx;
    u8 last_tx;
    u8 sent;
    u8 busy;
} ps2out;

// Initialize PS/2 output
// sm: State machine number (0 for keyboard, 2 for mouse)
// data_pin: GPIO for data line (clock is data_pin + 1)
void ps2out_init(ps2out* this, u8 sm, u8 data_pin, rx_callback rx_function);

// Extended init with explicit clock pin
void ps2out_init_ex(ps2out* this, u8 sm, u8 data_pin, u8 clk_pin, rx_callback rx_function);

// Process PS/2 tasks (send queued data, receive host commands)
void ps2out_task(ps2out* this);

// Queue packet for sending (packet[0] = length, packet[1..] = data)
void ps2out_send(ps2out* this, u8 len);

// Check if PS/2 bus is busy
bool ps2out_is_busy(void);

#endif // PS2OUT_H

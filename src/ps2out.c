/*
 * Hecate - PS/2 PIO Communication Driver
 *
 * Low-level driver for bidirectional PS/2 communication using the RP2040's
 * Programmable I/O (PIO) subsystem. Uses a single state machine per PS/2 port
 * that handles both TX and RX.
 *
 * Based on ps2x2pico by No0ne.
 *
 * Features:
 *   - Single state machine handles both TX and RX
 *   - Asynchronous packet transmission via queue
 *   - Host command reception with parity checking
 *   - Automatic resend on transmission failure
 *
 * SPDX-License-Identifier: MIT
 */

#include "ps2out.h"
#include "ps2out.pio.h"

static s8 ps2out_prg = -1;

static u32 ps2_frame(u8 byte) {
    bool parity = 1;
    for (u8 i = 0; i < 8; i++) {
        parity = parity ^ (byte >> i & 1);
    }
    return ((1 << 10) | (parity << 9) | (byte << 1)) ^ 0x7ff;
}

void ps2out_send(ps2out* this, u8 len) {
    this->packet[0] = len;
    queue_try_add(&this->packets, &this->packet);
}

void ps2out_init(ps2out* this, u8 sm, u8 data_pin, rx_callback rx_function) {
    this->sm = sm;
    this->data_pin = data_pin;
    this->rx_function = rx_function;
    this->last_rx = 0;
    this->last_tx = 0;
    this->sent = 0;
    this->busy = 0;

    queue_init(&this->packets, 9, 32);

    // Add program once, share between keyboard and mouse
    if (ps2out_prg == -1) {
        ps2out_prg = pio_add_program(pio1, &ps2out_program);
    }

    ps2out_program_init(pio1, sm, ps2out_prg, data_pin);
}

bool ps2out_is_busy(void) {
    // Check if any state machine has its busy flag set
    return pio_interrupt_get(pio1, 0) || pio_interrupt_get(pio1, 1);
}

void ps2out_task(ps2out* this) {
    u8 packet[9];

    // Check busy flag (IRQ 0 relative to state machine)
    bool is_busy = pio_interrupt_get(pio1, this->sm);
    
    if (this->busy) {
        this->busy--;
        if (is_busy) {
            this->busy = 0;
        }
    }

    // Check if we can send: not busy, both lines high, and have data to send
    if (!this->busy && !is_busy && 
        gpio_get(this->data_pin) && gpio_get(this->data_pin + 1) && 
        queue_try_peek(&this->packets, &packet)) {
        
        if (this->sent == packet[0]) {
            queue_try_remove(&this->packets, &packet);
            this->sent = 0;
        } else {
            this->sent++;
            this->last_tx = packet[this->sent];
            this->busy = 100;
            pio_sm_put(pio1, this->sm, ps2_frame(this->last_tx));
        }
    }

    // Check for TX failure (IRQ 4 relative to state machine)
    if (pio_interrupt_get(pio1, this->sm + 4)) {
        if (this->sent > 0) this->sent--;
        pio_interrupt_clear(pio1, this->sm + 4);
        pio_interrupt_clear(pio1, this->sm);
    }

    // Check for received data from host
    if (!pio_sm_is_rx_fifo_empty(pio1, this->sm)) {
        u32 fifo = pio_sm_get(pio1, this->sm) >> 23;

        // Verify parity
        bool parity = 1;
        for (u8 i = 0; i < 8; i++) {
            parity = parity ^ (fifo >> i & 1);
        }

        if (parity != (fifo >> 8)) {
            // Parity error, request resend
            pio_sm_put(pio1, this->sm, ps2_frame(0xfe));
            return;
        }

        if ((fifo & 0xff) == 0xfe) {
            // Host requested resend
            pio_sm_put(pio1, this->sm, ps2_frame(this->last_tx));
            return;
        }

        // Clear pending packets when host sends command
        while (queue_try_remove(&this->packets, &packet));
        this->sent = 0;

        // Call the receive callback
        (*this->rx_function)(fifo, this->last_rx);
        this->last_rx = fifo;
    }
}

/*
 * Hecate - LED Driver
 *
 * Supports both RP2040-Zero (WS2812 RGB LED on GPIO16) and
 * Raspberry Pi Pico (simple LED on GPIO25).
 *
 * Board detection: If WS2812_PIN is defined, use RGB LED.
 * Otherwise use standard GPIO LED on PICO_DEFAULT_LED_PIN.
 *
 * SPDX-License-Identifier: MIT
 */

#include "led.h"
#include "pico/stdlib.h"
#include "hardware/pio.h"
#include "hardware/clocks.h"

// WS2812 LED pin for RP2040-Zero
#ifndef WS2812_PIN
// Check if this is an RP2040-Zero by looking for typical pin
// RP2040-Zero has WS2812 on GPIO16
#if defined(PICO_DEFAULT_WS2812_PIN)
#define WS2812_PIN PICO_DEFAULT_WS2812_PIN
#define USE_WS2812 1
#else
#define USE_WS2812 0
#endif
#else
#define USE_WS2812 1
#endif

// Standard LED pin for Pico
#ifndef LED_PIN
#ifdef PICO_DEFAULT_LED_PIN
#define LED_PIN PICO_DEFAULT_LED_PIN
#else
#define LED_PIN 25
#endif
#endif

// LED state
static bool led_kb_connected = false;
static bool led_ms_connected = false;
static uint32_t led_blink_until = 0;

#if USE_WS2812

// Simple WS2812 driver using bit-banging
// Timing: T0H=0.4us, T1H=0.8us, T0L=0.85us, T1L=0.45us
// At 125MHz, 1 cycle = 8ns

static inline void ws2812_send_bit(uint pin, bool bit) {
    if (bit) {
        gpio_put(pin, 1);
        __asm volatile ("nop; nop; nop; nop; nop; nop; nop; nop; nop; nop;"
                        "nop; nop; nop; nop; nop; nop; nop; nop; nop; nop;"
                        "nop; nop; nop; nop; nop; nop; nop; nop; nop; nop;"
                        "nop; nop; nop; nop; nop; nop; nop; nop; nop; nop;"
                        "nop; nop; nop; nop; nop; nop; nop; nop; nop; nop;"
                        "nop; nop; nop; nop; nop; nop; nop; nop; nop; nop;"
                        "nop; nop; nop; nop; nop; nop; nop; nop; nop; nop;"
                        "nop; nop; nop; nop; nop; nop; nop; nop; nop; nop;");
        gpio_put(pin, 0);
        __asm volatile ("nop; nop; nop; nop; nop; nop; nop; nop; nop; nop;"
                        "nop; nop; nop; nop; nop; nop; nop; nop; nop; nop;"
                        "nop; nop; nop; nop; nop; nop; nop; nop; nop; nop;"
                        "nop; nop; nop; nop; nop; nop; nop; nop; nop; nop;");
    } else {
        gpio_put(pin, 1);
        __asm volatile ("nop; nop; nop; nop; nop; nop; nop; nop; nop; nop;"
                        "nop; nop; nop; nop; nop; nop; nop; nop; nop; nop;"
                        "nop; nop; nop; nop; nop; nop; nop; nop; nop; nop;"
                        "nop; nop; nop; nop; nop; nop; nop; nop; nop; nop;");
        gpio_put(pin, 0);
        __asm volatile ("nop; nop; nop; nop; nop; nop; nop; nop; nop; nop;"
                        "nop; nop; nop; nop; nop; nop; nop; nop; nop; nop;"
                        "nop; nop; nop; nop; nop; nop; nop; nop; nop; nop;"
                        "nop; nop; nop; nop; nop; nop; nop; nop; nop; nop;"
                        "nop; nop; nop; nop; nop; nop; nop; nop; nop; nop;"
                        "nop; nop; nop; nop; nop; nop; nop; nop; nop; nop;"
                        "nop; nop; nop; nop; nop; nop; nop; nop; nop; nop;"
                        "nop; nop; nop; nop; nop; nop; nop; nop; nop; nop;");
    }
}

static void ws2812_send_byte(uint pin, uint8_t byte) {
    for (int i = 7; i >= 0; i--) {
        ws2812_send_bit(pin, (byte >> i) & 1);
    }
}

// Send GRB color to WS2812
static void ws2812_set_color(uint8_t r, uint8_t g, uint8_t b) {
    uint32_t save = save_and_disable_interrupts();
    ws2812_send_byte(WS2812_PIN, g);
    ws2812_send_byte(WS2812_PIN, r);
    ws2812_send_byte(WS2812_PIN, b);
    restore_interrupts(save);
}

#endif // USE_WS2812

void led_init(void) {
#if USE_WS2812
    gpio_init(WS2812_PIN);
    gpio_set_dir(WS2812_PIN, GPIO_OUT);
    gpio_put(WS2812_PIN, 0);
    sleep_us(100); // Reset
    ws2812_set_color(0, 0, 0); // Off initially
#else
    gpio_init(LED_PIN);
    gpio_set_dir(LED_PIN, GPIO_OUT);
    gpio_put(LED_PIN, 0);
#endif
}

void led_set_connected(bool keyboard, bool mouse) {
    led_kb_connected = keyboard;
    led_ms_connected = mouse;
}

void led_blink_activity(void) {
    // Blink for 50ms
    led_blink_until = time_us_32() + 50000;
}

void led_task(void) {
    bool connected = led_kb_connected || led_ms_connected;
    bool blinking = time_us_32() < led_blink_until;
    
#if USE_WS2812
    // RGB LED: Green when connected, Blue on activity, Off when disconnected
    if (blinking) {
        ws2812_set_color(0, 0, 32); // Blue on keypress
    } else if (connected) {
        ws2812_set_color(0, 32, 0); // Green when connected
    } else {
        ws2812_set_color(0, 0, 0);  // Off
    }
#else
    // Simple LED: On when connected, blink off briefly on activity
    if (blinking) {
        gpio_put(LED_PIN, 0); // Blink off on keypress
    } else if (connected) {
        gpio_put(LED_PIN, 1); // On when connected
    } else {
        gpio_put(LED_PIN, 0); // Off when disconnected
    }
#endif
}

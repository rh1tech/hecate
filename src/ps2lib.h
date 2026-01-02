#ifndef ps2lib_H
#define ps2lib_H

#include "pico/stdlib.h"
#include "hardware/pio.h"

#define PS2_CLK_PIN 12
#define PS2_DATA_PIN 11

void ps2lib_init();
void ps2lib_send_byte(uint8_t byte);
void ps2lib_press_key(uint8_t key);
void ps2lib_release_key(uint8_t key);
void ps2lib_press_extended(uint16_t key);
void ps2lib_release_extended(uint16_t key);
void ps2lib_send_combo(uint8_t modifier, uint8_t key);
void ps2lib_send_extended_combo(uint16_t modifier, uint8_t key);
bool ps2lib_is_busy();
void ps2lib_poll();

#endif // ps2lib_H

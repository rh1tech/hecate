#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/clocks.h"
#include "ps2lib.h"
#include "scancodes.h"
#include "hid_to_ps2.h"
#include "usb_keyboard.h"
#include "pio_usb.h"
#include "tusb.h"

#define LED_PIN 25

// Callback from USB keyboard driver when a key event occurs
void on_keyboard_event(uint8_t modifiers, uint8_t keycode, bool pressed) {
    printf("Key event: mod=0x%02X, key=0x%02X, %s\n", 
           modifiers, keycode, pressed ? "pressed" : "released");
    
    // Convert HID keycode to PS/2 and send
    send_ps2_key(keycode, pressed);
}

// Core 1: USB host task
void core1_main(void) {
    sleep_ms(10);
    
    // Configure PIO-USB
    pio_usb_configuration_t pio_cfg = PIO_USB_DEFAULT_CONFIG;
    pio_cfg.pin_dp = USB1_DP_PIN;
    
    // Initialize TinyUSB host with PIO-USB
    tuh_configure(BOARD_TUH_RHPORT, TUH_CFGID_RPI_PIO_USB_CONFIGURATION, &pio_cfg);
    tuh_init(BOARD_TUH_RHPORT);
    
    printf("USB Host started on Core 1 (GPIO %d/%d)\n", USB1_DP_PIN, USB1_DP_PIN + 1);
    
    while (true) {
        tuh_task();
    }
}

int main() {
    // Set system clock to 120MHz for USB timing
    set_sys_clock_khz(120000, true);
    
    // Initialize stdio for USB serial
    stdio_init_all();

    // Initialize the GPIO pin for LED
    gpio_init(LED_PIN);
    gpio_set_dir(LED_PIN, GPIO_OUT);

    // Initialize PS/2 keyboard emulation (uses PIO)
    ps2lib_init();

    // Wait a bit for USB serial to enumerate
    sleep_ms(1000);
    
    printf("\n=================================\n");
    printf("USB to PS/2 Keyboard Converter\n");
    printf("=================================\n");
    printf("PS/2: CLK=GPIO%d, DATA=GPIO%d\n", PS2_CLK_PIN, PS2_DATA_PIN);
    printf("USB:  D+=GPIO%d, D-=GPIO%d\n", USB1_DP_PIN, USB1_DP_PIN + 1);
    printf("\n");
    
    // Start USB host on Core 1
    multicore_reset_core1();
    multicore_launch_core1(core1_main);
    
    // Main loop on Core 0 - blink LED to show we're alive
    while (true) {
        gpio_put(LED_PIN, 1);
        sleep_ms(500);
        gpio_put(LED_PIN, 0);
        sleep_ms(500);
    }

    return 0;
}

//--------------------------------------------------------------------
// TinyUSB HID Host Callbacks
//--------------------------------------------------------------------

// Previous keyboard state for detecting changes
static uint8_t prev_modifiers = 0;
static uint8_t prev_keys[6] = {0};

static bool key_in_array(uint8_t keycode, const uint8_t *array, size_t len) {
    for (size_t i = 0; i < len; i++) {
        if (array[i] == keycode) return true;
    }
    return false;
}

void tuh_hid_mount_cb(uint8_t dev_addr, uint8_t instance, uint8_t const *desc_report, uint16_t desc_len) {
    (void)desc_report;
    (void)desc_len;
    
    uint8_t const itf_protocol = tuh_hid_interface_protocol(dev_addr, instance);
    printf("HID mounted: dev=%d inst=%d protocol=%d\n", dev_addr, instance, itf_protocol);
    
    // Only handle keyboard (protocol 1)
    if (itf_protocol == HID_ITF_PROTOCOL_KEYBOARD) {
        printf("Keyboard detected!\n");
        if (!tuh_hid_receive_report(dev_addr, instance)) {
            printf("Failed to request report\n");
        }
    }
}

void tuh_hid_umount_cb(uint8_t dev_addr, uint8_t instance) {
    printf("HID unmounted: dev=%d inst=%d\n", dev_addr, instance);
    // Reset state
    prev_modifiers = 0;
    memset(prev_keys, 0, sizeof(prev_keys));
}

void tuh_hid_report_received_cb(uint8_t dev_addr, uint8_t instance, uint8_t const *report, uint16_t len) {
    uint8_t const itf_protocol = tuh_hid_interface_protocol(dev_addr, instance);
    
    if (itf_protocol == HID_ITF_PROTOCOL_KEYBOARD && len >= 8) {
        uint8_t modifiers = report[0];
        const uint8_t *keys = &report[2];
        
        // Check modifier changes
        uint8_t mod_changed = modifiers ^ prev_modifiers;
        if (mod_changed) {
            if (mod_changed & 0x01) on_keyboard_event(modifiers, 0xE0, modifiers & 0x01); // L Ctrl
            if (mod_changed & 0x02) on_keyboard_event(modifiers, 0xE1, modifiers & 0x02); // L Shift
            if (mod_changed & 0x04) on_keyboard_event(modifiers, 0xE2, modifiers & 0x04); // L Alt
            if (mod_changed & 0x08) on_keyboard_event(modifiers, 0xE3, modifiers & 0x08); // L GUI
            if (mod_changed & 0x10) on_keyboard_event(modifiers, 0xE4, modifiers & 0x10); // R Ctrl
            if (mod_changed & 0x20) on_keyboard_event(modifiers, 0xE5, modifiers & 0x20); // R Shift
            if (mod_changed & 0x40) on_keyboard_event(modifiers, 0xE6, modifiers & 0x40); // R Alt
            if (mod_changed & 0x80) on_keyboard_event(modifiers, 0xE7, modifiers & 0x80); // R GUI
        }
        
        // Check for released keys
        for (int i = 0; i < 6; i++) {
            if (prev_keys[i] && !key_in_array(prev_keys[i], keys, 6)) {
                on_keyboard_event(modifiers, prev_keys[i], false);
            }
        }
        
        // Check for pressed keys
        for (int i = 0; i < 6; i++) {
            if (keys[i] && !key_in_array(keys[i], prev_keys, 6)) {
                on_keyboard_event(modifiers, keys[i], true);
            }
        }
        
        prev_modifiers = modifiers;
        memcpy(prev_keys, keys, 6);
    }
    
    // Request next report
    tuh_hid_receive_report(dev_addr, instance);
}

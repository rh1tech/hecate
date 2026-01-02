#include "usb_keyboard.h"
#include "pio_usb.h"
#include "pio_usb_host.h"
#include "pico/stdlib.h"
#include <stdio.h>
#include <string.h>

// Previous keyboard state for detecting changes
static uint8_t prev_modifiers = 0;
static uint8_t prev_keys[6] = {0};

// USB device pointers
static usb_device_t *usb_device1 = NULL;
static usb_device_t *usb_device2 = NULL;

void usb_keyboard_init(void) {
    // Configure PIO-USB for host mode
    pio_usb_configuration_t pio_cfg = PIO_USB_DEFAULT_CONFIG;
    pio_cfg.pin_dp = USB1_DP_PIN;
    
    // Initialize USB host on PIO
    tuh_configure(0, TUH_CFGID_RPI_PIO_USB_CONFIGURATION, &pio_cfg);
    tuh_init(0);
    
    printf("USB Host initialized on GPIO %d/%d\n", USB1_DP_PIN, USB1_DP_PIN + 1);
}

// Check if a keycode exists in array
static bool key_in_array(uint8_t keycode, const uint8_t *array, size_t len) {
    for (size_t i = 0; i < len; i++) {
        if (array[i] == keycode) return true;
    }
    return false;
}

// Process a HID keyboard report
static void process_keyboard_report(uint8_t const *report) {
    uint8_t modifiers = report[0];
    const uint8_t *keys = &report[2];  // Skip modifier and reserved byte
    
    // Check modifier changes
    uint8_t mod_changed = modifiers ^ prev_modifiers;
    if (mod_changed) {
        // Left Control
        if (mod_changed & 0x01) on_keyboard_event(modifiers, 0xE0, modifiers & 0x01);
        // Left Shift
        if (mod_changed & 0x02) on_keyboard_event(modifiers, 0xE1, modifiers & 0x02);
        // Left Alt
        if (mod_changed & 0x04) on_keyboard_event(modifiers, 0xE2, modifiers & 0x04);
        // Left GUI
        if (mod_changed & 0x08) on_keyboard_event(modifiers, 0xE3, modifiers & 0x08);
        // Right Control
        if (mod_changed & 0x10) on_keyboard_event(modifiers, 0xE4, modifiers & 0x10);
        // Right Shift
        if (mod_changed & 0x20) on_keyboard_event(modifiers, 0xE5, modifiers & 0x20);
        // Right Alt
        if (mod_changed & 0x40) on_keyboard_event(modifiers, 0xE6, modifiers & 0x40);
        // Right GUI
        if (mod_changed & 0x80) on_keyboard_event(modifiers, 0xE7, modifiers & 0x80);
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
    
    // Save current state
    prev_modifiers = modifiers;
    memcpy(prev_keys, keys, 6);
}

void usb_keyboard_task(void) {
    tuh_task();
}

// TinyUSB callbacks for HID
void tuh_hid_mount_cb(uint8_t dev_addr, uint8_t instance, uint8_t const *desc_report, uint16_t desc_len) {
    printf("HID device mounted: dev_addr=%d, instance=%d\n", dev_addr, instance);
    
    // Request to receive reports
    if (!tuh_hid_receive_report(dev_addr, instance)) {
        printf("Failed to request HID report\n");
    }
}

void tuh_hid_umount_cb(uint8_t dev_addr, uint8_t instance) {
    printf("HID device unmounted: dev_addr=%d, instance=%d\n", dev_addr, instance);
}

void tuh_hid_report_received_cb(uint8_t dev_addr, uint8_t instance, uint8_t const *report, uint16_t len) {
    // Process keyboard report (8 bytes: modifier, reserved, key[6])
    if (len >= 8) {
        process_keyboard_report(report);
    }
    
    // Request next report
    tuh_hid_receive_report(dev_addr, instance);
}

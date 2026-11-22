/*
 * Hecate - USB to PS/2 Keyboard and Mouse Converter
 *
 * This file contains the main entry point and USB HID host implementation.
 * It handles USB device enumeration, HID report parsing, and dispatches
 * keyboard and mouse events to the respective PS/2 emulation drivers.
 *
 * Features:
 *   - Dual PIO-USB host ports (GPIO 2/3 and GPIO 4/5)
 *   - Native USB host port (Type-C) in hybrid mode
 *   - HID report descriptor parsing using pico-hid-host library
 *   - NKRO (N-Key Rollover) keyboard support
 *   - IntelliMouse and IntelliMouse Explorer support
 *   - USB gamepad/joystick support
 *   - XInput (Xbox controller) support
 *   - USB hub support for connecting multiple devices
 *
 * SPDX-License-Identifier: MIT
 */

#include <string.h>
#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "bsp/board_api.h"
#include "ps2_keyboard.h"
#include "ps2_mouse.h"
#include "led.h"
#include "pio_usb.h"
#include "tusb.h"

// HID Host parsing library
#include "hid_rip.h"
#include "hid_host_info.h"
#include "hid_host_joy.h"
#include "hid_host_mouse.h"
#include "xinput_host.h"

// Dual PIO-USB Host GPIO configuration
// Port 0: GPIO 2 (D+) / GPIO 3 (D-)
// Port 1: GPIO 4 (D+) / GPIO 5 (D-) - added via pio_usb_host_add_port
#define USB0_DP_PIN 2
#define USB1_DP_PIN 4

//--------------------------------------------------------------------
// HID Instance Tracking
//--------------------------------------------------------------------

#define MAX_BOOT 6
#define MAX_NKRO 16
#define MAX_REPORT 8

typedef struct {
    u8 report_count;
    tuh_hid_report_info_plus_t report_info[MAX_REPORT];
    u8 dev_addr;
    u8 modifiers;
    u8 boot[MAX_BOOT];
    u8 nkro[MAX_NKRO];
    bool leds;
    bool is_mouse;
    bool is_joystick;
} hid_instance_t;

static hid_instance_t hid_info[CFG_TUH_HID];

// Connection tracking for LED
static u8 kb_connected_count = 0;
static u8 ms_connected_count = 0;
static u8 joy_connected_count = 0;

// LED sync variables
static u8 kb_inst_loop = 0;
static u8 kb_last_dev = 0;
extern u8 kb_set_led;

//--------------------------------------------------------------------
// Keyboard Report Handling
//--------------------------------------------------------------------

static void handle_keyboard_report(u8 instance, u8 const* report, u16 len) {
    // Process modifier changes
    if (report[0] != hid_info[instance].modifiers) {
        led_blink_activity();
        for (u8 i = 0; i < 8; i++) {
            if ((report[0] >> i & 1) != (hid_info[instance].modifiers >> i & 1)) {
                ps2_keyboard_send_key(i + HID_KEY_CONTROL_LEFT, report[0] >> i & 1);
            }
        }
        hid_info[instance].modifiers = report[0];
    }

    const u8* keys = report + 1;
    u16 keys_len = len - 1;

    // Skip reserved byte if present (standard boot protocol has it)
    if (keys_len >= 7) {
        keys++;
        keys_len--;
    }

    // NKRO handling (report length > 12 bytes, typically bitmap)
    if (keys_len > 12 && keys_len < 31) {
        bool key_changed = false;
        for (u8 i = 0; i < keys_len && i < MAX_NKRO; i++) {
            for (u8 j = 0; j < 8; j++) {
                if ((keys[i] >> j & 1) != (hid_info[instance].nkro[i] >> j & 1)) {
                    key_changed = true;
                    ps2_keyboard_send_key(i * 8 + j, keys[i] >> j & 1);
                }
            }
        }
        if (key_changed) led_blink_activity();
        memcpy(hid_info[instance].nkro, keys, keys_len > MAX_NKRO ? MAX_NKRO : keys_len);
        return;
    }

    // Boot protocol / 6KRO handling
    if (keys_len >= 6) {
        bool key_changed = false;

        // Check for released keys
        for (u8 i = 0; i < MAX_BOOT; i++) {
            if (hid_info[instance].boot[i]) {
                bool brk = true;
                for (u8 j = 0; j < MAX_BOOT; j++) {
                    if (hid_info[instance].boot[i] == keys[j]) {
                        brk = false;
                        break;
                    }
                }
                if (brk) {
                    key_changed = true;
                    ps2_keyboard_send_key(hid_info[instance].boot[i], false);
                }
            }
        }

        // Check for pressed keys
        for (u8 i = 0; i < MAX_BOOT; i++) {
            if (keys[i]) {
                bool make = true;
                for (u8 j = 0; j < MAX_BOOT; j++) {
                    if (keys[i] == hid_info[instance].boot[j]) {
                        make = false;
                        break;
                    }
                }
                if (make) {
                    key_changed = true;
                    ps2_keyboard_send_key(keys[i], true);
                }
            }
        }

        if (key_changed) led_blink_activity();
        memcpy(hid_info[instance].boot, keys, MAX_BOOT);
    }
}

//--------------------------------------------------------------------
// Mouse Report Handling
//--------------------------------------------------------------------

static void handle_mouse_report_boot(u8 const* report, u16 len) {
    static u8 prev_buttons = 0;
    if (report == NULL || len < 3) return;

    if (report[0] != prev_buttons) {
        led_blink_activity();
        prev_buttons = report[0];
    }

    ps2_mouse_send_movement(report[0], report[1], report[2], len > 3 ? report[3] : 0);
}

static void handle_mouse_report_parsed(tusb_hid_simple_mouse_t* mouse) {
    static u8 prev_buttons = 0;
    u8 buttons = mouse->values.buttons & 0xFF;

    if (buttons != prev_buttons) {
        led_blink_activity();
        prev_buttons = buttons;
    }

    // Clamp values to signed 8-bit range
    s8 x = (mouse->values.x1 > 127) ? 127 : (mouse->values.x1 < -127) ? -127 : mouse->values.x1;
    s8 y = (mouse->values.y1 > 127) ? 127 : (mouse->values.y1 < -127) ? -127 : mouse->values.y1;
    s8 w = (mouse->values.w1 > 127) ? 127 : (mouse->values.w1 < -127) ? -127 : mouse->values.w1;

    ps2_mouse_send_movement(buttons, x, y, w);
}

//--------------------------------------------------------------------
// Joystick Report Handling
//--------------------------------------------------------------------

// Joystick data is available via tuh_hid_get_simple_joysticks()
// Applications can query joystick state in the main loop if needed.
// For PS/2 output, joysticks could be mapped to keyboard keys or mouse,
// but that requires configuration which is beyond the scope of basic support.

//--------------------------------------------------------------------
// HID Host Info Handlers
//--------------------------------------------------------------------

static void handle_keyboard_unmount(tusb_hid_host_info_t* info) {
    (void)info;
    if (kb_connected_count > 0) kb_connected_count--;
    led_set_connected(kb_connected_count > 0, ms_connected_count > 0);
}

static void handle_mouse_unmount(tusb_hid_host_info_t* info) {
    tuh_hid_free_simple_mice_for_instance(info->key.elements.dev_addr, info->key.elements.instance);
    if (ms_connected_count > 0) ms_connected_count--;
    led_set_connected(kb_connected_count > 0, ms_connected_count > 0);
}

static void handle_joystick_unmount(tusb_hid_host_info_t* info) {
    tuh_hid_free_simple_joysticks_for_instance(info->key.elements.dev_addr, info->key.elements.instance);
    if (joy_connected_count > 0) joy_connected_count--;
}

static void handle_kbd_report_cb(tusb_hid_host_info_t* info, const uint8_t* report, uint8_t report_length, uint8_t report_id) {
    (void)report_id;
    u8 instance = info->key.elements.instance;
    handle_keyboard_report(instance, report, report_length);
}

static void handle_mouse_report_cb(tusb_hid_host_info_t* info, const uint8_t* report, uint8_t report_length, uint8_t report_id) {
    tusb_hid_simple_mouse_t* mouse = tuh_hid_get_simple_mouse(
        info->key.elements.dev_addr,
        info->key.elements.instance,
        report_id);

    if (mouse != NULL) {
        tusb_hid_simple_mouse_process_report(mouse, report, report_length);
        handle_mouse_report_parsed(mouse);
    }
}

static void handle_joystick_report_cb(tusb_hid_host_info_t* info, const uint8_t* report, uint8_t report_length, uint8_t report_id) {
    tusb_hid_simple_joystick_t* joystick = tuh_hid_get_simple_joystick(
        info->key.elements.dev_addr,
        info->key.elements.instance,
        report_id);

    if (joystick != NULL) {
        tusb_hid_simple_joystick_process_report(joystick, report, report_length);
        // Joystick data is now available via tuh_hid_get_simple_joysticks()
    }
}

//--------------------------------------------------------------------
// LED Sync Callback
//--------------------------------------------------------------------

s64 kb_led_sync_callback(alarm_id_t id, void *user_data) {
    (void)id;
    (void)user_data;

    if (hid_info[kb_inst_loop].leds && kb_last_dev != hid_info[kb_inst_loop].dev_addr) {
        tuh_hid_set_report(hid_info[kb_inst_loop].dev_addr, kb_inst_loop, 0, HID_REPORT_TYPE_OUTPUT, &kb_set_led, 1);
        kb_last_dev = hid_info[kb_inst_loop].dev_addr;
    }

    kb_inst_loop++;

    if (kb_inst_loop == CFG_TUH_HID) {
        kb_inst_loop = 0;
        kb_last_dev = 0;
        return 0;
    }

    return 1000;
}

//--------------------------------------------------------------------
// TinyUSB HID Host Callbacks
//--------------------------------------------------------------------

void tuh_hid_mount_cb(u8 dev_addr, u8 instance, u8 const* desc_report, u16 desc_len) {
    if (desc_report == NULL && desc_len == 0) {
        return;
    }

    hid_interface_protocol_enum_t itf_protocol = tuh_hid_interface_protocol(dev_addr, instance);

    // Parse report descriptor
    hid_info[instance].report_count = tuh_hid_parse_report_descriptor_plus(
        hid_info[instance].report_info, MAX_REPORT, desc_report, desc_len);

    bool has_report_id = hid_info[instance].report_count > 1 ||
                         (hid_info[instance].report_count > 0 && hid_info[instance].report_info[0].report_id > 0);

    // Determine device type and set up handlers
    if (itf_protocol == HID_ITF_PROTOCOL_KEYBOARD) {
        // Boot protocol keyboard
        tuh_hid_allocate_info(dev_addr, instance, has_report_id, handle_kbd_report_cb, handle_keyboard_unmount);
        hid_info[instance].dev_addr = dev_addr;
        hid_info[instance].modifiers = 0;
        memset(hid_info[instance].boot, 0, MAX_BOOT);
        memset(hid_info[instance].nkro, 0, MAX_NKRO);
        hid_info[instance].leds = true;
        hid_info[instance].is_mouse = false;
        hid_info[instance].is_joystick = false;
        kb_connected_count++;
        led_set_connected(kb_connected_count > 0, ms_connected_count > 0);
    }
    else if (itf_protocol == HID_ITF_PROTOCOL_MOUSE) {
        // Boot protocol mouse - use boot protocol for reliability
        tuh_hid_set_protocol(dev_addr, instance, HID_PROTOCOL_BOOT);
        hid_info[instance].leds = false;
        hid_info[instance].is_mouse = true;
        hid_info[instance].is_joystick = false;
        ms_connected_count++;
        led_set_connected(kb_connected_count > 0, ms_connected_count > 0);
    }
    else {
        // Non-boot protocol device - parse report descriptor to determine type
        for (u8 i = 0; i < hid_info[instance].report_count; i++) {
            tuh_hid_report_info_plus_t* rpt = &hid_info[instance].report_info[i];

            if (rpt->usage_page == HID_USAGE_PAGE_DESKTOP) {
                switch (rpt->usage) {
                    case HID_USAGE_DESKTOP_KEYBOARD:
                        tuh_hid_allocate_info(dev_addr, instance, has_report_id, handle_kbd_report_cb, handle_keyboard_unmount);
                        hid_info[instance].dev_addr = dev_addr;
                        hid_info[instance].modifiers = 0;
                        memset(hid_info[instance].boot, 0, MAX_BOOT);
                        memset(hid_info[instance].nkro, 0, MAX_NKRO);
                        hid_info[instance].leds = true;
                        hid_info[instance].is_mouse = false;
                        hid_info[instance].is_joystick = false;
                        kb_connected_count++;
                        led_set_connected(kb_connected_count > 0, ms_connected_count > 0);
                        break;

                    case HID_USAGE_DESKTOP_MOUSE:
                        if (tuh_hid_allocate_info(dev_addr, instance, has_report_id, handle_mouse_report_cb, handle_mouse_unmount)) {
                            tuh_hid_mouse_parse_report_descriptor(desc_report, desc_len, dev_addr, instance);
                        }
                        hid_info[instance].leds = false;
                        hid_info[instance].is_mouse = true;
                        hid_info[instance].is_joystick = false;
                        ms_connected_count++;
                        led_set_connected(kb_connected_count > 0, ms_connected_count > 0);
                        break;

                    case HID_USAGE_DESKTOP_GAMEPAD:
                    case HID_USAGE_DESKTOP_JOYSTICK:
                        if (tuh_hid_allocate_info(dev_addr, instance, has_report_id, handle_joystick_report_cb, handle_joystick_unmount)) {
                            tuh_hid_joystick_parse_report_descriptor(desc_report, desc_len, dev_addr, instance);
                        }
                        hid_info[instance].leds = false;
                        hid_info[instance].is_mouse = false;
                        hid_info[instance].is_joystick = true;
                        joy_connected_count++;
                        break;

                    default:
                        break;
                }
            }
        }
    }

    // Request to receive reports
    tuh_hid_receive_report(dev_addr, instance);
}

void tuh_hid_umount_cb(u8 dev_addr, u8 instance) {
    // Free host info and invoke unmount handlers
    tuh_hid_free_info(dev_addr, instance);

    hid_info[instance].dev_addr = 0;
    hid_info[instance].leds = false;
    hid_info[instance].is_mouse = false;
    hid_info[instance].is_joystick = false;
}

void tuh_hid_report_received_cb(u8 dev_addr, u8 instance, u8 const* report, u16 len) {
    // Get host info for this device
    tusb_hid_host_info_t* info = tuh_hid_get_info(dev_addr, instance);

    if (info != NULL) {
        // Route through registered handler
        u8 report_id = 0;
        const u8* report_data = report;
        u16 report_len = len;

        if (info->has_report_id && len > 0) {
            report_id = report[0];
            report_data = report + 1;
            report_len = len - 1;
        }

        info->handler(info, report_data, report_len, report_id);
    }
    else {
        // Fallback for boot protocol devices
        hid_interface_protocol_enum_t itf_protocol = tuh_hid_interface_protocol(dev_addr, instance);

        if (itf_protocol == HID_ITF_PROTOCOL_MOUSE) {
            handle_mouse_report_boot(report, len);
        }
        else if (itf_protocol == HID_ITF_PROTOCOL_KEYBOARD) {
            handle_keyboard_report(instance, report, len);
        }
    }

    // Continue receiving reports
    tuh_hid_receive_report(dev_addr, instance);
}

//--------------------------------------------------------------------
// XInput (Xbox Controller) Support
//--------------------------------------------------------------------

// Register XInput driver with TinyUSB
usbh_class_driver_t const* usbh_app_driver_get_cb(uint8_t* driver_count) {
    *driver_count = 1;
    return &usbh_xinput_driver;
}

void tuh_xinput_mount_cb(uint8_t dev_addr, uint8_t instance, const xinputh_interface_t *xinput_itf) {
    // Wait for wireless controller connection packet
    if (xinput_itf->type == XBOX360_WIRELESS && xinput_itf->connected == false) {
        tuh_xinput_receive_report(dev_addr, instance);
        return;
    }

    // Initialize Xbox 360 wired controllers
    if (xinput_itf->type == XBOX360_WIRED) {
        uint8_t dummy[20];
        tusb_control_request_t const request = {
            .bmRequestType_bit = {
                .recipient = TUSB_REQ_RCPT_INTERFACE,
                .type = TUSB_REQ_TYPE_VENDOR,
                .direction = TUSB_DIR_IN
            },
            .bRequest = 0x01,
            .wValue = 0x100,
            .wIndex = 0x00,
            .wLength = 20
        };
        tuh_xfer_t xfer = {
            .daddr = dev_addr,
            .ep_addr = 0,
            .setup = &request,
            .buffer = dummy,
            .complete_cb = NULL,
            .user_data = 0
        };
        tuh_control_xfer(&xfer);
    }

    tuh_xinput_set_led(dev_addr, instance, 0, true);
    tuh_xinput_set_led(dev_addr, instance, 1, true);
    tuh_xinput_set_rumble(dev_addr, instance, 0, 0, true);
    tuh_xinput_receive_report(dev_addr, instance);

    // Allocate joystick slot for XInput device
    tusb_hid_simple_joystick_t* joystick = tuh_hid_obtain_simple_joystick(dev_addr, instance, 0);
    if (joystick != NULL) {
        joystick->axis_x1.logical_min = -32768/2;
        joystick->axis_x1.logical_max = 32768/2;
        joystick->axis_y1.logical_min = -32768/2;
        joystick->axis_y1.logical_max = 32768/2;
        joystick->axis_x2.logical_min = -32768/2;
        joystick->axis_x2.logical_max = 32768/2;
        joystick->axis_y2.logical_min = -32768/2;
        joystick->axis_y2.logical_max = 32768/2;
        joystick->hat.length = 1;
        joystick->hat.logical_max = 7;
    }

    joy_connected_count++;
}

void tuh_xinput_umount_cb(uint8_t dev_addr, uint8_t instance) {
    tuh_hid_free_simple_joysticks_for_instance(dev_addr, instance);
    if (joy_connected_count > 0) joy_connected_count--;
}

// Hat direction lookup table
static const uint8_t xinput_hat_table[16] = {8, 0, 4, 8, 6, 7, 5, 8, 2, 1, 3, 8, 8, 8, 8, 8};

void tuh_xinput_report_received_cb(uint8_t dev_addr, uint8_t instance, xinputh_interface_t const* xid_itf, uint16_t len) {
    (void)len;
    const xinput_gamepad_t *p = &xid_itf->pad;

    if (xid_itf->connected && xid_itf->new_pad_data) {
        tusb_hid_simple_joystick_t* joystick = tuh_hid_get_simple_joystick(dev_addr, instance, 0);

        if (joystick != NULL) {
            tusb_hid_simple_joystick_values_t* values = &joystick->values;
            values->x1 = p->sThumbLX;
            values->y1 = -p->sThumbLY;
            values->x2 = p->sThumbRX;
            values->y2 = -p->sThumbRY;
            values->hat = xinput_hat_table[p->wButtons & 0x0F];

            values->buttons = 0;
            if (p->wButtons & XINPUT_GAMEPAD_A) values->buttons |= 0x01;
            if (p->wButtons & XINPUT_GAMEPAD_B) values->buttons |= 0x02;
            if (p->wButtons & XINPUT_GAMEPAD_X) values->buttons |= 0x04;
            if (p->wButtons & XINPUT_GAMEPAD_Y) values->buttons |= 0x08;
            if (p->wButtons & XINPUT_GAMEPAD_LEFT_SHOULDER) values->buttons |= 0x10;
            if (p->wButtons & XINPUT_GAMEPAD_RIGHT_SHOULDER) values->buttons |= 0x20;
            if (p->wButtons & XINPUT_GAMEPAD_BACK) values->buttons |= 0x40;
            if (p->wButtons & XINPUT_GAMEPAD_START) values->buttons |= 0x80;
            if (p->wButtons & XINPUT_GAMEPAD_LEFT_THUMB) values->buttons |= 0x100;
            if (p->wButtons & XINPUT_GAMEPAD_RIGHT_THUMB) values->buttons |= 0x200;
            if (p->wButtons & XINPUT_GAMEPAD_GUIDE) values->buttons |= 0x400;

            joystick->has_values = true;
            joystick->updated = board_millis();
        }
    }

    tuh_xinput_receive_report(dev_addr, instance);
}

//--------------------------------------------------------------------
// Main
//--------------------------------------------------------------------

int main() {
    // Set system clock to 120MHz for USB timing
    set_sys_clock_khz(120000, true);

    // Initialize board
    board_init();

    // Initialize LED driver
    led_init();

    // Initialize PS/2 keyboard emulation
    ps2_keyboard_init();

    // Initialize PS/2 mouse emulation
    ps2_mouse_init();

    // Configure HID protocol - use report protocol for better device support
    tuh_hid_set_default_protocol(HID_PROTOCOL_REPORT);

#if CFG_TUH_RPI_HYBRID_USB
    // Hybrid mode: Native USB (Type-C) + PIO-USB

    // Initialize native USB host on rhport 0 (Type-C port)
    tuh_init(0);

    // Configure PIO-USB on rhport 1 (GPIO 2/3)
    pio_usb_configuration_t pio_cfg = PIO_USB_DEFAULT_CONFIG;
    pio_cfg.pin_dp = USB0_DP_PIN;
    tuh_configure(1, TUH_CFGID_RPI_PIO_USB_CONFIGURATION, &pio_cfg);
    tuh_init(1);

    // Run a few USB task cycles to let ports stabilize
    for (int i = 0; i < 100; i++) {
        tuh_task();
        sleep_ms(1);
    }

    // Add PIO-USB port 1 (GPIO 4/5) as additional root port
    pio_usb_host_add_port(USB1_DP_PIN, PIO_USB_PINOUT_DPDM);

#else
    // PIO-USB only mode

    // Configure USB port 0 (GPIO 2/3)
    pio_usb_configuration_t pio_cfg = PIO_USB_DEFAULT_CONFIG;
    pio_cfg.pin_dp = USB0_DP_PIN;
    tuh_configure(0, TUH_CFGID_RPI_PIO_USB_CONFIGURATION, &pio_cfg);
    tuh_init(0);

    // Run a few USB task cycles to let port 0 stabilize
    for (int i = 0; i < 100; i++) {
        tuh_task();
        sleep_ms(1);
    }

    // Add USB port 1 (GPIO 4/5) as additional root port
    pio_usb_host_add_port(USB1_DP_PIN, PIO_USB_PINOUT_DPDM);

#endif

    // Allow ports to enumerate connected devices
    for (int i = 0; i < 100; i++) {
        tuh_task();
        sleep_ms(1);
    }

    // Main loop
    while (true) {
        tuh_task();
        ps2_keyboard_task();
        ps2_mouse_task();
        led_task();
    }

    return 0;
}

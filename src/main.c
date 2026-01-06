/*
 * Hecate - USB to PS/2 Keyboard and Mouse Converter
 *
 * This file contains the main entry point and USB HID host implementation.
 * It handles USB device enumeration, HID report parsing, and dispatches
 * keyboard and mouse events to the respective PS/2 emulation drivers.
 *
 * Features:
 *   - Dual PIO-USB host ports (GPIO 2/3 and GPIO 4/5)
 *   - HID report descriptor parsing for non-boot protocol devices
 *   - NKRO (N-Key Rollover) keyboard support
 *   - IntelliMouse and IntelliMouse Explorer support
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

// Dual PIO-USB Host GPIO configuration
// Port 0: GPIO 2 (D+) / GPIO 3 (D-)
// Port 1: GPIO 4 (D+) / GPIO 5 (D-) - added via pio_usb_host_add_port
#define USB0_DP_PIN 2
#define USB1_DP_PIN 4

//--------------------------------------------------------------------
// HID Report Parsing Structures
//--------------------------------------------------------------------

#define MAX_BOOT 6
#define MAX_NKRO 16
#define MAX_REPORT 8
#define MAX_REPORT_ITEMS 32

typedef struct {
    u16 page;
    u16 usage;
} hid_usage_t;

typedef struct {
    s32 min;
    s32 max;
} hid_minmax_t;

typedef struct {
    hid_usage_t usage;
    hid_minmax_t logical;
} hid_report_item_attributes_t;

typedef struct {
    u16 bit_offset;
    u8 bit_size;
    u8 item_type;
    hid_report_item_attributes_t attributes;
} hid_report_item_t;

typedef struct {
    u8 report_id;
    u8 usage;
    u16 usage_page;
    u8 num_items;
    hid_report_item_t item[MAX_REPORT_ITEMS];
} hid_report_info_t;

typedef struct {
    const hid_report_item_t *x;
    const hid_report_item_t *y;
    const hid_report_item_t *z;
    const hid_report_item_t *lb;
    const hid_report_item_t *mb;
    const hid_report_item_t *rb;
    const hid_report_item_t *bw;
    const hid_report_item_t *fw;
} ms_items_t;

typedef struct {
    u8 report_count;
    hid_report_info_t report_info[MAX_REPORT];
    u8 dev_addr;
    u8 modifiers;
    u8 boot[MAX_BOOT];
    u8 nkro[MAX_NKRO];
    bool leds;
    bool is_mouse;
} hid_instance_t;

static hid_instance_t hid_info[CFG_TUH_HID];
static ms_items_t ms_items;

// Connection tracking for LED
static u8 kb_connected_count = 0;
static u8 ms_connected_count = 0;

// LED sync variables
static u8 kb_inst_loop = 0;
static u8 kb_last_dev = 0;
extern u8 kb_set_led;

//--------------------------------------------------------------------
// HID Report Descriptor Parsing
//--------------------------------------------------------------------

static bool hid_parse_find_bit_item_by_page(hid_report_info_t* info, u8 type, u16 page, u8 bit, const hid_report_item_t **item) {
    for (u8 i = 0; i < info->num_items; i++) {
        if (info->item[i].item_type == type && info->item[i].attributes.usage.page == page) {
            if (item) {
                if (i + bit < info->num_items && info->item[i + bit].item_type == type && 
                    info->item[i + bit].attributes.usage.page == page) {
                    *item = &info->item[i + bit];
                } else {
                    return false;
                }
            }
            return true;
        }
    }
    return false;
}

static bool hid_parse_find_item_by_usage(hid_report_info_t* info, u8 type, u16 usage, const hid_report_item_t **item) {
    for (u8 i = 0; i < info->num_items; i++) {
        if (info->item[i].item_type == type && info->item[i].attributes.usage.usage == usage) {
            if (item) {
                *item = &info->item[i];
            }
            return true;
        }
    }
    return false;
}

static bool hid_parse_get_item_value(const hid_report_item_t *item, const u8 *report, u8 len, s32 *value) {
    (void)len;
    if (item == NULL || report == NULL) return false;
    u8 boffs = item->bit_offset & 0x07;
    u8 pos = 8 - boffs;
    u8 offs = item->bit_offset >> 3;
    u32 mask = ~(0xffffffff << item->bit_size);
    s32 val = report[offs++] >> boffs;
    while (item->bit_size > pos) {
        val |= (report[offs++] << pos);
        pos += 8;
    }
    val &= mask;
    if (item->attributes.logical.min < 0) {
        if (val & (1 << (item->bit_size - 1))) {
            val |= (0xffffffff << item->bit_size);
        }
    }
    *value = val;
    return true;
}

static s8 to_signed_value8(const hid_report_item_t *item, const u8 *report, u16 len) {
    s32 value = 0;
    if (hid_parse_get_item_value(item, report, len, &value)) {
        value = (value > 127) ? 127 : (value < -127) ? -127 : value;
    }
    return value;
}

static bool to_bit_value(const hid_report_item_t *item, const u8 *report, u16 len) {
    s32 value = 0;
    hid_parse_get_item_value(item, report, len, &value);
    return value ? true : false;
}

static u8 hid_parse_report_descriptor(hid_report_info_t* report_info_arr, u8 arr_count, u8 const* desc_report, u16 desc_len) {
    union __attribute__((packed)) {
        u8 byte;
        struct __attribute__((packed)) {
            u8 size: 2;
            u8 type: 2;
            u8 tag: 4;
        };
    } header;

    memset(report_info_arr, 0, arr_count * sizeof(hid_report_info_t));

    u8 report_num = 0;
    hid_report_info_t* info = report_info_arr;

    u16 ri_global_usage_page = 0;
    s32 ri_global_logical_min = 0;
    s32 ri_global_logical_max = 0;
    u8 ri_report_count = 0;
    u8 ri_report_size = 0;
    u8 ri_report_usage_count = 0;
    u8 ri_collection_depth = 0;

    while (desc_len && report_num < arr_count) {
        header.byte = *desc_report++;
        desc_len--;

        u8 const tag = header.tag;
        u8 const type = header.type;
        u8 const size = header.size;

        u32 data;
        s32 sdata;
        switch (size) {
            case 1: data = desc_report[0]; sdata = ((data & 0x80) ? 0xffffff00 : 0) | data; break;
            case 2: data = (desc_report[1] << 8) | desc_report[0]; sdata = ((data & 0x8000) ? 0xffff0000 : 0) | data; break;
            case 3: data = (desc_report[3] << 24) | (desc_report[2] << 16) | (desc_report[1] << 8) | desc_report[0]; sdata = data; break;
            default: data = 0; sdata = 0;
        }

        u16 offset;
        switch (type) {
            case 0: // Main
                switch (tag) {
                    case 8: // Input
                    case 9: // Output
                    case 11: // Feature
                        offset = (info->num_items == 0) ? 0 : (info->item[info->num_items - 1].bit_offset + info->item[info->num_items - 1].bit_size);
                        if (ri_report_usage_count > ri_report_count) {
                            info->num_items += ri_report_usage_count - ri_report_count;
                        }
                        for (u8 i = 0; i < ri_report_count; i++) {
                            if (info->num_items + i < MAX_REPORT_ITEMS) {
                                info->item[info->num_items + i].bit_offset = offset;
                                info->item[info->num_items + i].bit_size = ri_report_size;
                                info->item[info->num_items + i].item_type = tag;
                                info->item[info->num_items + i].attributes.logical.min = ri_global_logical_min;
                                info->item[info->num_items + i].attributes.logical.max = ri_global_logical_max;
                                info->item[info->num_items + i].attributes.usage.page = ri_global_usage_page;
                                if (ri_report_usage_count != ri_report_count && ri_report_usage_count > 0) {
                                    if (i >= ri_report_usage_count) {
                                        info->item[info->num_items + i].attributes.usage = info->item[info->num_items + i - 1].attributes.usage;
                                    }
                                }
                            }
                            offset += ri_report_size;
                        }
                        info->num_items += ri_report_count;
                        ri_report_usage_count = 0;
                        break;

                    case 10: // Collection
                        ri_report_usage_count = 0;
                        ri_report_count = 0;
                        ri_collection_depth++;
                        break;

                    case 12: // End Collection
                        ri_collection_depth--;
                        if (ri_collection_depth == 0) {
                            info++;
                            report_num++;
                        }
                        break;
                }
                break;

            case 1: // Global
                switch (tag) {
                    case 0: // Usage Page
                        if (ri_collection_depth == 0) {
                            info->usage_page = data;
                        }
                        ri_global_usage_page = data;
                        break;
                    case 1: ri_global_logical_min = sdata; break;
                    case 2: ri_global_logical_max = sdata; break;
                    case 7: ri_report_size = data; break;
                    case 8: info->report_id = data; break;
                    case 9: ri_report_count = data; break;
                }
                break;

            case 2: // Local
                if (tag == 0) { // Usage
                    if (ri_collection_depth == 0) {
                        info->usage = data;
                    } else {
                        if (ri_report_usage_count < MAX_REPORT_ITEMS) {
                            info->item[info->num_items + ri_report_usage_count].attributes.usage.usage = data;
                            ri_report_usage_count++;
                        }
                    }
                }
                break;
        }

        desc_report += size;
        desc_len -= size;
    }

    return report_num;
}

//--------------------------------------------------------------------
// Mouse Report Handling
//--------------------------------------------------------------------

static void ms_setup(hid_report_info_t *info) {
    memset(&ms_items, 0, sizeof(ms_items_t));
    hid_parse_find_item_by_usage(info, 8, HID_USAGE_DESKTOP_X, &ms_items.x);
    hid_parse_find_item_by_usage(info, 8, HID_USAGE_DESKTOP_Y, &ms_items.y);
    hid_parse_find_item_by_usage(info, 8, HID_USAGE_DESKTOP_WHEEL, &ms_items.z);
    hid_parse_find_bit_item_by_page(info, 8, HID_USAGE_PAGE_BUTTON, 0, &ms_items.lb);
    hid_parse_find_bit_item_by_page(info, 8, HID_USAGE_PAGE_BUTTON, 1, &ms_items.rb);
    hid_parse_find_bit_item_by_page(info, 8, HID_USAGE_PAGE_BUTTON, 2, &ms_items.mb);
    hid_parse_find_bit_item_by_page(info, 8, HID_USAGE_PAGE_BUTTON, 3, &ms_items.bw);
    hid_parse_find_bit_item_by_page(info, 8, HID_USAGE_PAGE_BUTTON, 4, &ms_items.fw);
}

static void ms_report_receive(u8 const* report, u16 len) {
    static u8 prev_buttons = 0;
    u8 buttons = 0;
    s8 x, y, z;

    if (to_bit_value(ms_items.lb, report, len)) buttons |= 0x01;
    if (to_bit_value(ms_items.rb, report, len)) buttons |= 0x02;
    if (to_bit_value(ms_items.mb, report, len)) buttons |= 0x04;
    if (to_bit_value(ms_items.bw, report, len)) buttons |= 0x08;
    if (to_bit_value(ms_items.fw, report, len)) buttons |= 0x10;

    x = to_signed_value8(ms_items.x, report, len);
    y = to_signed_value8(ms_items.y, report, len);
    z = to_signed_value8(ms_items.z, report, len);

    // Blink LED on button press/release (not movement)
    if (buttons != prev_buttons) {
        led_blink_activity();
        prev_buttons = buttons;
    }

    ps2_mouse_send_movement(buttons, x, y, z);
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

    hid_interface_protocol_enum_t hid_if_proto = tuh_hid_interface_protocol(dev_addr, instance);

    hid_info[instance].report_count = hid_parse_report_descriptor(hid_info[instance].report_info, MAX_REPORT, desc_report, desc_len);

    if (tuh_hid_receive_report(dev_addr, instance)) {
        if (hid_if_proto == HID_ITF_PROTOCOL_MOUSE) {
            hid_info[instance].leds = false;
            hid_info[instance].is_mouse = true;
            ms_connected_count++;
        } else {
            hid_info[instance].dev_addr = dev_addr;
            hid_info[instance].modifiers = 0;
            memset(hid_info[instance].boot, 0, MAX_BOOT);
            memset(hid_info[instance].nkro, 0, MAX_NKRO);
            hid_info[instance].leds = true;
            hid_info[instance].is_mouse = false;
            kb_connected_count++;
        }
        led_set_connected(kb_connected_count > 0, ms_connected_count > 0);
    }
}

void tuh_hid_umount_cb(u8 dev_addr, u8 instance) {
    (void)dev_addr;
    if (hid_info[instance].is_mouse) {
        if (ms_connected_count > 0) ms_connected_count--;
    } else if (hid_info[instance].leds) {
        if (kb_connected_count > 0) kb_connected_count--;
    }
    hid_info[instance].dev_addr = 0;
    hid_info[instance].leds = false;
    hid_info[instance].is_mouse = false;
    led_set_connected(kb_connected_count > 0, ms_connected_count > 0);
}

void tuh_hid_report_received_cb(u8 dev_addr, u8 instance, u8 const* report, u16 len) {
    u8 const rpt_count = hid_info[instance].report_count;
    hid_report_info_t *rpt_infos = hid_info[instance].report_info;
    hid_report_info_t *rpt_info = NULL;

    if (rpt_count == 1 && rpt_infos[0].report_id == 0) {
        rpt_info = &rpt_infos[0];
    } else {
        u8 const rpt_id = report[0];
        for (u8 i = 0; i < rpt_count; i++) {
            if (rpt_id == rpt_infos[i].report_id) {
                rpt_info = &rpt_infos[i];
                break;
            }
        }
        report++;
        len--;
    }

    if (!rpt_info) {
        tuh_hid_receive_report(dev_addr, instance);
        return;
    }
    
    tuh_hid_receive_report(dev_addr, instance);

    // Handle mouse reports
    if (tuh_hid_interface_protocol(dev_addr, instance) == HID_ITF_PROTOCOL_MOUSE) {
        if (tuh_hid_get_protocol(dev_addr, instance) == HID_PROTOCOL_BOOT) {
            // Boot protocol mouse - blink on button press only
            static u8 prev_buttons = 0;
            if (report[0] != prev_buttons) {
                led_blink_activity();
                prev_buttons = report[0];
            }
            ps2_mouse_send_movement(report[0], report[1], report[2], len > 3 ? report[3] : 0);
        } else if (rpt_info->usage_page == HID_USAGE_PAGE_DESKTOP && rpt_info->usage == HID_USAGE_DESKTOP_MOUSE) {
            ms_setup(rpt_info);
            ms_report_receive(report, len);
        }
        return;
    }

    // Handle keyboard reports
    if (rpt_info->usage_page != HID_USAGE_PAGE_DESKTOP || rpt_info->usage != HID_USAGE_DESKTOP_KEYBOARD) {
        return;
    }

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

    report++;
    len--;

    // NKRO handling (len > 12 and < 31)
    if (len > 12 && len < 31) {
        bool key_changed = false;
        for (u8 i = 0; i < len && i < MAX_NKRO; i++) {
            for (u8 j = 0; j < 8; j++) {
                if ((report[i] >> j & 1) != (hid_info[instance].nkro[i] >> j & 1)) {
                    key_changed = true;
                    ps2_keyboard_send_key(i * 8 + j, report[i] >> j & 1);
                }
            }
        }
        if (key_changed) led_blink_activity();
        memcpy(hid_info[instance].nkro, report, len > MAX_NKRO ? MAX_NKRO : len);
        return;
    }

    // Boot protocol / 6KRO handling
    switch (len) {
        case 8:
        case 7:
            report++;
            // fall through
        case 6: {
            bool key_changed = false;
            // Check for released keys
            for (u8 i = 0; i < MAX_BOOT; i++) {
                if (hid_info[instance].boot[i]) {
                    bool brk = true;
                    for (u8 j = 0; j < MAX_BOOT; j++) {
                        if (hid_info[instance].boot[i] == report[j]) {
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
                if (report[i]) {
                    bool make = true;
                    for (u8 j = 0; j < MAX_BOOT; j++) {
                        if (report[i] == hid_info[instance].boot[j]) {
                            make = false;
                            break;
                        }
                    }
                    if (make) {
                        key_changed = true;
                        ps2_keyboard_send_key(report[i], true);
                    }
                }
            }

            if (key_changed) led_blink_activity();
            memcpy(hid_info[instance].boot, report, MAX_BOOT);
            return;
        }
    }
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

    // Configure and initialize PIO-USB host
    tuh_hid_set_default_protocol(HID_PROTOCOL_REPORT);
    
    // Configure USB port 0 (GPIO 2/3)
    pio_usb_configuration_t pio_cfg = PIO_USB_DEFAULT_CONFIG;
    pio_cfg.pin_dp = USB0_DP_PIN;
    tuh_configure(0, TUH_CFGID_RPI_PIO_USB_CONFIGURATION, &pio_cfg);
    tuh_init(0);
    
    // Add USB port 1 (GPIO 4/5) as additional root port
    // This shares the PIO state machines with port 0
    pio_usb_host_add_port(USB1_DP_PIN, PIO_USB_PINOUT_DPDM);
    
    // Main loop
    while (true) {
        tuh_task();
        ps2_keyboard_task();
        ps2_mouse_task();
        led_task();
    }

    return 0;
}

/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2024 Fruit-Bat
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *
 */

#include "hid_app.h"
#include "bsp/board.h"
#include "tusb.h"
#include "hid_host_joy.h"
#include "hid_host_mouse.h"
#include "hid_host_info.h"
#include "xinput_host.h"

void tuh_hid_app_startup() {
  board_init();

  tuh_hid_set_default_protocol(HID_PROTOCOL_REPORT);

  // init host stack on configured roothub port
  tuh_init(BOARD_TUH_RHPORT);

  if (board_init_after_tusb) {
    board_init_after_tusb();
  }
}

#define BIT0 0x01
#define BIT1 0x02
#define BIT2 0x04
#define BIT3 0x08
#define BIT4 0x10
#define BIT5 0x20
#define BIT6 0x40
#define BIT7 0x80


//--------------------------------------------------------------------+
// MACRO TYPEDEF CONSTANT ENUM DECLARATION
//--------------------------------------------------------------------+

// If your host terminal support ansi escape code such as TeraTerm
// it can be use to simulate mouse cursor movement within terminal
#define USE_ANSI_ESCAPE   0

#define MAX_REPORT  4

TU_ATTR_WEAK void process_kbd_report(hid_keyboard_report_t const *report, hid_keyboard_report_t const *prev_report) {}
TU_ATTR_WEAK void process_kbd_mount(uint8_t dev_addr, uint8_t instance) {}
TU_ATTR_WEAK void process_kbd_unmount(uint8_t dev_addr, uint8_t instance) {}
TU_ATTR_WEAK void process_mouse_mount(uint8_t dev_addr, uint8_t instance) {}
TU_ATTR_WEAK void process_mouse_unmount(uint8_t dev_addr, uint8_t instance) {}
TU_ATTR_WEAK void process_mouse_report(hid_mouse_report_t const * report) {}

static void process_generic_report(uint8_t dev_addr, uint8_t instance, uint8_t const* report, uint16_t len);
//--------------------------------------------------------------------+
// HID Keyboard
//--------------------------------------------------------------------+

static void __not_in_flash_func(_process_kbd_report)(hid_keyboard_report_t const *report)
{
  static hid_keyboard_report_t prev_report = { 0, 0, {0} }; // previous report to check key released
  process_kbd_report(report, &prev_report);
  prev_report = *report;
}

void __not_in_flash_func(handle_kbd_report)(tusb_hid_host_info_t* info, const uint8_t* report, uint8_t report_length, uint8_t report_id)
{
  TU_LOG1("HID receive keyboard report\r\n");
  _process_kbd_report((hid_keyboard_report_t*)report);
}

void handle_keyboard_unmount(tusb_hid_host_info_t* info) {
  TU_LOG1("HID keyboard unmount\n");
  // Free up keybouard definitions
  process_kbd_unmount(info->key.elements.dev_addr, info->key.elements.instance);
}

//--------------------------------------------------------------------+
// HID Mouse
//--------------------------------------------------------------------+

void handle_mouse_unmount(tusb_hid_host_info_t* info) {
  TU_LOG1("HID mouse unmount\n");
  // Free up mouse definitions
  tuh_hid_free_simple_mice_for_instance(info->key.elements.dev_addr, info->key.elements.instance);

  process_mouse_unmount(info->key.elements.dev_addr, info->key.elements.instance);
}

void __not_in_flash_func(handle_mouse_report)(tusb_hid_host_info_t* info, const uint8_t* report, uint8_t report_length, uint8_t report_id)
{
  TU_LOG1("HID receive mouse report\r\n");
  tusb_hid_simple_mouse_t* simple_mouse = tuh_hid_get_simple_mouse(
    info->key.elements.dev_addr, 
    info->key.elements.instance, 
    report_id);
    
  if (simple_mouse != NULL) {
    tusb_hid_simple_mouse_process_report(simple_mouse, report, report_length);
    hid_mouse_report_t report;
    report.x = simple_mouse->values.x1;
    report.y = simple_mouse->values.y1;
    report.wheel = simple_mouse->values.w1;
    report.buttons = simple_mouse->values.buttons;
    process_mouse_report(&report);
  }
}

//--------------------------------------------------------------------+
// HID Joystick/Gamepad
//--------------------------------------------------------------------+

void __not_in_flash_func(handle_joystick_report)(tusb_hid_host_info_t* info, const uint8_t* report, uint8_t report_length, uint8_t report_id)
{ 
  TU_LOG1("HID receive joystick report\r\n");
  tusb_hid_simple_joystick_t* simple_joystick = tuh_hid_get_simple_joystick(
    info->key.elements.dev_addr, 
    info->key.elements.instance, 
    report_id);
    
  if (simple_joystick != NULL) {
    tusb_hid_simple_joystick_process_report(simple_joystick, report, report_length);
  }
}

void handle_joystick_unmount(tusb_hid_host_info_t* info) {
  TU_LOG1("HID joystick unmount\n");
  // Free up joystick definitions
  tuh_hid_free_simple_joysticks_for_instance(info->key.elements.dev_addr, info->key.elements.instance);
}

void handle_gamepad_report(tusb_hid_host_info_t* info, const uint8_t* report, uint8_t report_length, uint8_t report_id)
{
  TU_LOG1("HID receive gamepad report ");
  for(int i = 0; i < report_length; ++i) {
    TU_LOG2("%02x", report[i]);
  }
  TU_LOG2("\r\n");
}

//--------------------------------------------------------------------+
// TinyUSB Callbacks
//--------------------------------------------------------------------+

// Invoked when device with hid interface is mounted
// Report descriptor is also available for use. tuh_hid_parse_report_descriptor_plus()
// can be used to parse common/simple enough descriptor.
// Note: if report descriptor length > CFG_TUH_ENUMERATION_BUFSIZE, it will be skipped
// therefore report_desc = NULL, desc_len = 0
void tuh_hid_mount_cb(uint8_t dev_addr, uint8_t instance, uint8_t const* desc_report, uint16_t desc_len)
{
  printf("HID device address = %d, instance = %d is mounted\r\n", dev_addr, instance);

  printf("HID Report Description \r\n");
  for(int i = 0; i < desc_len; ++i) printf("%02X ", desc_report[i]);
  printf("\r\n");

  // Interface protocol (hid_interface_protocol_enum_t)
  const char* protocol_str[] = { "None", "Keyboard", "Mouse" };
  uint8_t const itf_protocol = tuh_hid_interface_protocol(dev_addr, instance);

  printf("HID Interface Protocol = %s\r\n", protocol_str[itf_protocol]);

  // By default host stack will use activate boot protocol on supported interface.
  // Therefore for this simple example, we only need to parse generic report descriptor (with built-in parser)
  if ( itf_protocol == HID_ITF_PROTOCOL_NONE || itf_protocol == HID_USAGE_DESKTOP_MOUSE)
  {
    tuh_hid_report_info_plus_t reports[MAX_REPORT];
    uint8_t report_count = tuh_hid_parse_report_descriptor_plus(reports, MAX_REPORT, desc_report, desc_len);
    printf("HID has %u reports \r\n", report_count);
    
    for (uint8_t i = 0; i < report_count; ++i) {
      tuh_hid_report_info_plus_t *report = &reports[i];
      bool has_report_id = report_count > 1 || (report[0].report_id > 0);
      
      printf("HID report usage_page=%d, usage=%d, has_report_id=%d dev_addr=%d instance=%d\n", report->usage_page, report->usage, has_report_id, dev_addr, instance);
      
      if (report->usage_page == HID_USAGE_PAGE_DESKTOP)
      {
        switch (report->usage)
        {
          case HID_USAGE_DESKTOP_KEYBOARD: {
            printf("HID receive keyboard report description dev_addr=%d instance=%d\r\n", dev_addr, instance);
            tuh_hid_allocate_info(dev_addr, instance, has_report_id, &handle_kbd_report, handle_keyboard_unmount);
            process_kbd_mount(dev_addr, instance);
            break;
          }
          case HID_USAGE_DESKTOP_GAMEPAD:
          case HID_USAGE_DESKTOP_JOYSTICK: {
            printf("HID receive joystick report description dev_addr=%d instance=%d\r\n", dev_addr, instance);
            if(tuh_hid_allocate_info(dev_addr, instance, has_report_id, &handle_joystick_report, handle_joystick_unmount)) {
              tuh_hid_joystick_parse_report_descriptor(desc_report, desc_len, dev_addr, instance);
            }
            break;
          }
          case HID_USAGE_DESKTOP_MOUSE: {
            printf("HID receive mouse report description dev_addr=%d instance=%d\r\n", dev_addr, instance);
            if(tuh_hid_allocate_info(dev_addr, instance, has_report_id, &handle_mouse_report, handle_mouse_unmount)) {
              tuh_hid_mouse_parse_report_descriptor(desc_report, desc_len, dev_addr, instance);
              process_mouse_mount(dev_addr, instance);
            }
            break;
          }

          default: {
            TU_LOG1("HID usage unknown usage:%d\r\n", report->usage);
            break;
          }
        }
      }
    }  
  }
  else if ( itf_protocol == HID_ITF_PROTOCOL_KEYBOARD )
  {
     tuh_hid_allocate_info(dev_addr, instance, false, handle_kbd_report, handle_keyboard_unmount);
     process_kbd_mount(dev_addr, instance);
  }
  else if ( itf_protocol == HID_ITF_PROTOCOL_MOUSE )
  {
    tuh_hid_allocate_info(dev_addr, instance, false, handle_mouse_report, handle_mouse_unmount);
    process_mouse_mount(dev_addr, instance);
  }
  
  // request to receive report
  // tuh_hid_report_received_cb() will be invoked when report is available
  if ( !tuh_hid_receive_report(dev_addr, instance) )
  {
    printf("Error: cannot request to receive report\r\n");
  }
}

// Invoked when device with hid interface is un-mounted
void tuh_hid_umount_cb(uint8_t dev_addr, uint8_t instance)
{
  printf("HID device address = %d, instance = %d is unmounted\r\n", dev_addr, instance);
  
  // Invoke unmount functions adn free up host info structure 
  tuh_hid_free_info(dev_addr, instance);
}

// Invoked when received report from device via interrupt endpoint
void __not_in_flash_func(tuh_hid_report_received_cb)(uint8_t dev_addr, uint8_t instance, uint8_t const* report, uint16_t len)
{
  uint8_t const itf_protocol = tuh_hid_interface_protocol(dev_addr, instance);

  switch (itf_protocol)
  {
    // case HID_ITF_PROTOCOL_KEYBOARD:
    //   TU_LOG2("HID receive boot keyboard report\r\n");
    //   _process_kbd_report((hid_keyboard_report_t*)report);
    // break;

    // case HID_ITF_PROTOCOL_MOUSE:
    //   TU_LOG2("HID receive boot mouse report\r\n");
    //   process_mouse_report( (hid_mouse_report_t const*) report );
    // break;

    default:
      TU_LOG2("HID receive generic report\r\n");
      // Generic report requires matching ReportID and contents with previous parsed report info
      process_generic_report(dev_addr, instance, report, len);
    break;
  }

  // continue to request to receive report
  if ( !tuh_hid_receive_report(dev_addr, instance) )
  {
    TU_LOG2("Error: cannot request to receive report\r\n");
  }
}

//--------------------------------------------------------------------+
// Generic Report
//--------------------------------------------------------------------+
static void __not_in_flash_func(process_generic_report)(uint8_t dev_addr, uint8_t instance, uint8_t const* report, uint16_t len)
{
  tusb_hid_host_info_t* info = tuh_hid_get_info(dev_addr, instance);
  
  if (info == NULL)
  {
    TU_LOG2("Couldn't find the host report info for dev_addr=%d instance=%d\r\n", dev_addr, instance);
    return;
  }
  
  uint8_t rpt_id = 0;

  if (info->has_report_id) {
    rpt_id = report[0];
    report++;
    len--;       
  }
  
  info->handler(info, report, len, rpt_id);
}

//--------------------------------------------------------------------+
// XINPUT Gamepad
//--------------------------------------------------------------------+
#define USB_XINPUT_JOYSTICK
#ifdef USB_XINPUT_JOYSTICK
usbh_class_driver_t const* usbh_app_driver_get_cb(uint8_t* driver_count) {
    *driver_count = 1;
    return &usbh_xinput_driver;
}

enum Direction {
    North = 0,
    NorthEast,
    East,
    SouthEast,
    South,
    SouthWest,
    West,
    NorthWest,
    None = 8
};
/*

        U
        | 
   L----+----R
        | 
        D

        N
        | 
   W----+----E
        | 
        S


#define XINPUT_GAMEPAD_DPAD_UP 0x0001
#define XINPUT_GAMEPAD_DPAD_DOWN 0x0002
#define XINPUT_GAMEPAD_DPAD_LEFT 0x0004
#define XINPUT_GAMEPAD_DPAD_RIGHT 0x0008

0x00,None
0x01,North      //XINPUT_GAMEPAD_DPAD_UP
0x02,South      //XINPUT_GAMEPAD_DPAD_DOWN
0x03,None
0x04,West       //XINPUT_GAMEPAD_DPAD_LEFT
0x05,NorthWest  //XINPUT_GAMEPAD_DPAD_UP|XINPUT_GAMEPAD_DPAD_LEFT
0x06,SouthWest  //XINPUT_GAMEPAD_DPAD_DOWN|XINPUT_GAMEPAD_DPAD_LEFT
0x07,None
0x08,East       //XINPUT_GAMEPAD_DPAD_RIGHT
0x09,NorthEast  //XINPUT_GAMEPAD_DPAD_UP|XINPUT_GAMEPAD_DPAD_RIGHT
0x0A,SouthEast  //XINPUT_GAMEPAD_DPAD_DOWN|XINPUT_GAMEPAD_DPAD_RIGHT
0x0B,None
0x0C,None
0x0D,None
0x0E,None
0x0F,None
*/

const uint8_t hattable[16]={None,North,South,None,West,NorthWest,SouthWest,None,East,NorthEast,SouthEast,None,None,None,None,None};

void __not_in_flash_func(tuh_xinput_report_received_cb)(uint8_t dev_addr, uint8_t instance, xinputh_interface_t const* xid_itf, uint16_t len)
{
    const xinput_gamepad_t *p = &xid_itf->pad;
  
    if (xid_itf->connected && xid_itf->new_pad_data) {
        TU_LOG1("HID receive joystick report\r\n");
        tusb_hid_simple_joystick_t* joystick = tuh_hid_get_simple_joystick(dev_addr, instance, 0);
            
        if (joystick != NULL) {
            tusb_hid_simple_joystick_values_t* values = &joystick->values;
            values->x1 = p->sThumbLX;
            values->y1 = - p->sThumbLY;
            values->x2 = p->sThumbRX;
            values->y2 = - p->sThumbRY;

            values->hat = hattable[p->wButtons & 0x0F];

            values->buttons=0;
            if (p->wButtons & XINPUT_GAMEPAD_A) values->buttons|=BIT0;
            if (p->wButtons & XINPUT_GAMEPAD_B) values->buttons|=BIT1;
            if (p->wButtons & XINPUT_GAMEPAD_X) values->buttons|=BIT2;
            if (p->wButtons & XINPUT_GAMEPAD_Y) values->buttons|=BIT3;
           
            joystick->has_values = true;
            joystick->updated = board_millis();
         }

        //update_joystate_xinput(p->wButtons, p->sThumbLX, p->sThumbLY, p->sThumbRX, p->sThumbRY, p->bLeftTrigger, p->bRightTrigger);
    }
    tuh_xinput_receive_report(dev_addr, instance);
}

void tuh_xinput_mount_cb(uint8_t dev_addr, uint8_t instance, const xinputh_interface_t *xinput_itf)
{
    TU_LOG1("XINPUT MOUNTED %02x %d\n", dev_addr, instance);
    // If this is a Xbox 360 Wireless controller we need to wait for a connection packet
    // on the in pipe before setting LEDs etc. So just start getting data until a controller is connected.
    if (xinput_itf->type == XBOX360_WIRELESS && xinput_itf->connected == false) {
        tuh_xinput_receive_report(dev_addr, instance);
        return;
    } else if (xinput_itf->type == XBOX360_WIRED) {
        /*
         * Some third-party Xbox 360-style controllers require this message to finish initialization.
         * Idea taken from Linux drivers/input/joystick/xpad.c
         */
        uint8_t dummy[20];
        tusb_control_request_t const request =
        {
            .bmRequestType_bit =
            {
                .recipient = TUSB_REQ_RCPT_INTERFACE,
                .type      = TUSB_REQ_TYPE_VENDOR,
                .direction = TUSB_DIR_IN
            },
            .bRequest = tu_htole16(0x01),
            .wValue   = tu_htole16(0x100),
            .wIndex   = tu_htole16(0x00),
            .wLength  = 20
        };
        tuh_xfer_t xfer =
        {
            .daddr       = dev_addr,
            .ep_addr     = 0,
            .setup       = &request,
            .buffer      = dummy,
            .complete_cb = NULL,
            .user_data   = 0
        };
        tuh_control_xfer(&xfer);
    }
    tuh_xinput_set_led(dev_addr, instance, 0, true);
    tuh_xinput_set_led(dev_addr, instance, 1, true);
    tuh_xinput_set_rumble(dev_addr, instance, 0, 0, true);
    tuh_xinput_receive_report(dev_addr, instance);

    tusb_hid_simple_joystick_t* joystick = tuh_hid_obtain_simple_joystick(dev_addr, instance, 0);

    if (joystick!=NULL) {
      joystick->axis_x1.logical_min=-32768/2;
      joystick->axis_x1.logical_max= 32768/2;
      joystick->axis_y1.logical_min=-32768/2;
      joystick->axis_y1.logical_max= 32768/2;
      joystick->axis_x2.logical_min=-32768/2;
      joystick->axis_x2.logical_max= 32768/2;
      joystick->axis_y2.logical_min=-32768/2;
      joystick->axis_y2.logical_max= 32768/2;
      joystick->hat.length = 1;
      joystick->hat.logical_max = 7;
    }
}

void tuh_xinput_umount_cb(uint8_t dev_addr, uint8_t instance)
{
    //printf("XINPUT UNMOUNTED %02x %d\n", dev_addr, instance);
    tuh_hid_free_simple_joysticks_for_instance(dev_addr, instance);
}

#endif
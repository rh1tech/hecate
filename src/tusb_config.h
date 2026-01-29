#ifndef TUSB_CONFIG_H
#define TUSB_CONFIG_H

//--------------------------------------------------------------------
// COMMON CONFIGURATION
//--------------------------------------------------------------------

#define CFG_TUSB_MCU                OPT_MCU_RP2040
#define CFG_TUSB_OS                 OPT_OS_PICO
#define CFG_TUSB_DEBUG              0

// Enable Host mode (PIO-USB for HID)
#define CFG_TUH_ENABLED             1

//--------------------------------------------------------------------
// PIO-USB Configuration
// Uses pio_usb_host_add_port() for dual USB ports on:
// Port 0: GPIO 2 (D+) / GPIO 3 (D-)
// Port 1: GPIO 4 (D+) / GPIO 5 (D-)
//--------------------------------------------------------------------

#define CFG_TUH_RPI_PIO_USB         1

// PIO-USB uses rhport 0 internally
#define CFG_TUSB_RHPORT0_MODE       (OPT_MODE_HOST | OPT_MODE_FULL_SPEED)

#ifndef BOARD_TUH_RHPORT
#define BOARD_TUH_RHPORT            0
#endif

#define CFG_TUH_MAX_SPEED           OPT_MODE_FULL_SPEED

//--------------------------------------------------------------------
// Host Driver Configuration
//--------------------------------------------------------------------

// Hub support (for keyboards/mice connected via hub)
#define CFG_TUH_HUB                 2

// Max device support (excluding hub device)
#define CFG_TUH_DEVICE_MAX          (3*CFG_TUH_HUB + 1)

// HID support - enough for keyboard + mouse + extras
#define CFG_TUH_HID                 (3*CFG_TUH_DEVICE_MAX)

//--------------------------------------------------------------------
// HID Configuration
//--------------------------------------------------------------------

#define CFG_TUH_HID_EPIN_BUFSIZE    128
#define CFG_TUH_HID_EPOUT_BUFSIZE   128

// Size of buffer to hold descriptors and other data used for enumeration
#define CFG_TUH_ENUMERATION_BUFSIZE 256

#endif // TUSB_CONFIG_H

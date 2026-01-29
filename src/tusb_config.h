#ifndef TUSB_CONFIG_H
#define TUSB_CONFIG_H

//--------------------------------------------------------------------
// COMMON CONFIGURATION
//--------------------------------------------------------------------

#define CFG_TUSB_MCU                OPT_MCU_RP2040
#define CFG_TUSB_OS                 OPT_OS_PICO
#define CFG_TUSB_DEBUG              0

// Enable Host mode
#define CFG_TUH_ENABLED             1

//--------------------------------------------------------------------
// Hybrid USB Configuration
//
// Enables both native USB (Type-C) and PIO-USB simultaneously:
//   rhport 0: Native RP2040 USB controller (Type-C port)
//   rhport 1: PIO-USB port 0 (GPIO 2/3)
//   rhport 2: PIO-USB port 1 (GPIO 4/5) - added via pio_usb_host_add_port
//--------------------------------------------------------------------

// Enable hybrid mode (native USB + PIO-USB)
#ifndef CFG_TUH_RPI_HYBRID_USB
#define CFG_TUH_RPI_HYBRID_USB      0
#endif

#if CFG_TUH_RPI_HYBRID_USB
    // Hybrid mode: Native USB on rhport 0, PIO-USB on rhport 1
    // Note: CFG_TUH_RPI_PIO_USB is NOT defined here because our hybrid
    // driver (hcd_hybrid.c) provides all HCD functions directly
    #define CFG_TUSB_RHPORT0_MODE   (OPT_MODE_HOST | OPT_MODE_FULL_SPEED)
    #define CFG_TUSB_RHPORT1_MODE   (OPT_MODE_HOST | OPT_MODE_FULL_SPEED)
    #define BOARD_TUH_RHPORT        0
#else
    // PIO-USB only mode (original configuration)
    #define CFG_TUH_RPI_PIO_USB     1
    #define CFG_TUSB_RHPORT0_MODE   (OPT_MODE_HOST | OPT_MODE_FULL_SPEED)
    #define BOARD_TUH_RHPORT        0
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

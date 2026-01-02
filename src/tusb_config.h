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
#define CFG_TUH_RPI_PIO_USB         1

// Max number of USB devices
#define CFG_TUH_DEVICE_MAX          2

// Hub support (for keyboards connected via hub)
#define CFG_TUH_HUB                 1
#define CFG_TUH_HUB_MAX             1

//--------------------------------------------------------------------
// HID Configuration
//--------------------------------------------------------------------

#define CFG_TUH_HID                 4
#define CFG_TUH_HID_EPIN_BUFSIZE    64
#define CFG_TUH_HID_EPOUT_BUFSIZE   64

//--------------------------------------------------------------------
// Buffer sizes
//--------------------------------------------------------------------

#define CFG_TUH_ENUMERATION_BUFSIZE 256

// RHPort number used for host (PIO-USB)
#define BOARD_TUH_RHPORT            0

#endif // TUSB_CONFIG_H

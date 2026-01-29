/*
 * Hecate - Hybrid HCD Driver (Native USB + PIO-USB)
 *
 * This driver enables simultaneous use of:
 *   - rhport 0: Native RP2040 USB controller (Type-C port)
 *   - rhport 1: PIO-USB port 0 (GPIO 2/3)
 *   - rhport 2: PIO-USB port 1 (GPIO 4/5)
 *
 * Based on TinyUSB's hcd_rp2040.c and hcd_pio_usb.c
 *
 * SPDX-License-Identifier: MIT
 */

#include "tusb_option.h"

#if CFG_TUH_ENABLED && (CFG_TUSB_MCU == OPT_MCU_RP2040) && CFG_TUH_RPI_HYBRID_USB

#include "pico.h"
#include "hardware/irq.h"
#include "hardware/resets.h"

// Native USB includes
#include "rp2040_usb.h"

// PIO-USB includes
#include "pio_usb.h"
#include "pio_usb_ll.h"

#include "osal/osal.h"
#include "host/hcd.h"
#include "host/usbh.h"

//--------------------------------------------------------------------+
// Port Mapping
//--------------------------------------------------------------------+
#define RHPORT_NATIVE     0
#define RHPORT_PIO_OFFSET 1
#define RHPORT_PIO(_x)    ((_x) - RHPORT_PIO_OFFSET)

#define IS_NATIVE_PORT(rhport)  ((rhport) == RHPORT_NATIVE)
#define IS_PIO_PORT(rhport)     ((rhport) >= RHPORT_PIO_OFFSET)

//--------------------------------------------------------------------+
// Native USB Data Structures
//--------------------------------------------------------------------+
#ifndef PICO_USB_HOST_INTERRUPT_ENDPOINTS
#define PICO_USB_HOST_INTERRUPT_ENDPOINTS (USB_MAX_ENDPOINTS - 1)
#endif
static_assert(PICO_USB_HOST_INTERRUPT_ENDPOINTS <= USB_MAX_ENDPOINTS, "");

static struct hw_endpoint ep_pool[1 + PICO_USB_HOST_INTERRUPT_ENDPOINTS];
#define epx (ep_pool[0])

enum {
    SIE_CTRL_BASE = USB_SIE_CTRL_SOF_EN_BITS      | USB_SIE_CTRL_KEEP_ALIVE_EN_BITS |
                    USB_SIE_CTRL_PULLDOWN_EN_BITS | USB_SIE_CTRL_EP0_INT_1BUF_BITS
};

//--------------------------------------------------------------------+
// PIO-USB Configuration
//--------------------------------------------------------------------+
static pio_usb_configuration_t pio_host_cfg = PIO_USB_DEFAULT_CONFIG;
static bool pio_usb_initialized = false;

//--------------------------------------------------------------------+
// Native USB Helper Functions
//--------------------------------------------------------------------+
static struct hw_endpoint *get_dev_ep(uint8_t dev_addr, uint8_t ep_addr) {
    uint8_t num = tu_edpt_number(ep_addr);
    if (num == 0) return &epx;

    for (uint32_t i = 1; i < TU_ARRAY_SIZE(ep_pool); i++) {
        struct hw_endpoint *ep = &ep_pool[i];
        if (ep->configured && (ep->dev_addr == dev_addr) && (ep->ep_addr == ep_addr)) return ep;
    }
    return NULL;
}

TU_ATTR_ALWAYS_INLINE static inline uint8_t dev_speed(void) {
    return (usb_hw->sie_status & USB_SIE_STATUS_SPEED_BITS) >> USB_SIE_STATUS_SPEED_LSB;
}

TU_ATTR_ALWAYS_INLINE static inline bool need_pre(uint8_t dev_addr) {
    return hcd_port_speed_get(RHPORT_NATIVE) != tuh_speed_get(dev_addr);
}

static void __tusb_irq_path_func(hw_xfer_complete)(struct hw_endpoint *ep, xfer_result_t xfer_result) {
    uint8_t dev_addr = ep->dev_addr;
    uint8_t ep_addr = ep->ep_addr;
    uint xferred_len = ep->xferred_len;
    hw_endpoint_reset_transfer(ep);
    hcd_event_xfer_complete(dev_addr, ep_addr, xferred_len, xfer_result, true);
}

static void __tusb_irq_path_func(_handle_buff_status_bit)(uint bit, struct hw_endpoint *ep) {
    usb_hw_clear->buf_status = bit;
    assert(ep->active);
    bool done = hw_endpoint_xfer_continue(ep);
    if (done) {
        hw_xfer_complete(ep, XFER_RESULT_SUCCESS);
    }
}

static void __tusb_irq_path_func(hw_handle_buff_status)(void) {
    uint32_t remaining_buffers = usb_hw->buf_status;

    uint bit = 0b1;
    if (remaining_buffers & bit) {
        remaining_buffers &= ~bit;
        _handle_buff_status_bit(bit, &epx);
    }

    for (uint i = 1; i <= USB_HOST_INTERRUPT_ENDPOINTS && remaining_buffers; i++) {
        for (uint j = 0; j < 2; j++) {
            bit = 1 << (i * 2 + j);
            if (remaining_buffers & bit) {
                remaining_buffers &= ~bit;
                _handle_buff_status_bit(bit, &ep_pool[i]);
            }
        }
    }
}

static void __tusb_irq_path_func(hw_trans_complete)(void) {
    if (usb_hw->sie_ctrl & USB_SIE_CTRL_SEND_SETUP_BITS) {
        struct hw_endpoint *ep = &epx;
        assert(ep->active);
        ep->xferred_len = 8;
        hw_xfer_complete(ep, XFER_RESULT_SUCCESS);
    }
}

static void __tusb_irq_path_func(hcd_rp2040_irq)(void) {
    uint32_t status = usb_hw->ints;
    uint32_t handled = 0;

    if (status & USB_INTS_HOST_CONN_DIS_BITS) {
        handled |= USB_INTS_HOST_CONN_DIS_BITS;
        if (dev_speed()) {
            hcd_event_device_attach(RHPORT_NATIVE, true);
        } else {
            hcd_event_device_remove(RHPORT_NATIVE, true);
        }
        usb_hw_clear->sie_status = USB_SIE_STATUS_SPEED_BITS;
    }

    if (status & USB_INTS_STALL_BITS) {
        handled |= USB_INTS_STALL_BITS;
        usb_hw_clear->sie_status = USB_SIE_STATUS_STALL_REC_BITS;
        hw_xfer_complete(&epx, XFER_RESULT_STALLED);
    }

    if (status & USB_INTS_BUFF_STATUS_BITS) {
        handled |= USB_INTS_BUFF_STATUS_BITS;
        hw_handle_buff_status();
    }

    if (status & USB_INTS_TRANS_COMPLETE_BITS) {
        handled |= USB_INTS_TRANS_COMPLETE_BITS;
        usb_hw_clear->sie_status = USB_SIE_STATUS_TRANS_COMPLETE_BITS;
        hw_trans_complete();
    }

    if (status & USB_INTS_ERROR_RX_TIMEOUT_BITS) {
        handled |= USB_INTS_ERROR_RX_TIMEOUT_BITS;
        usb_hw_clear->sie_status = USB_SIE_STATUS_RX_TIMEOUT_BITS;
    }

    if (status & USB_INTS_ERROR_DATA_SEQ_BITS) {
        usb_hw_clear->sie_status = USB_SIE_STATUS_DATA_SEQ_ERROR_BITS;
        panic("Data Seq Error\n");
    }
}

static struct hw_endpoint *_next_free_interrupt_ep(void) {
    for (uint i = 1; i < TU_ARRAY_SIZE(ep_pool); i++) {
        struct hw_endpoint *ep = &ep_pool[i];
        if (!ep->configured) {
            ep->interrupt_num = (uint8_t)(i - 1);
            return ep;
        }
    }
    return NULL;
}

static struct hw_endpoint *_hw_endpoint_allocate(uint8_t transfer_type) {
    struct hw_endpoint *ep = NULL;

    if (transfer_type != TUSB_XFER_CONTROL) {
        ep = _next_free_interrupt_ep();
        assert(ep);
        ep->buffer_control = &usbh_dpram->int_ep_buffer_ctrl[ep->interrupt_num].ctrl;
        ep->endpoint_control = &usbh_dpram->int_ep_ctrl[ep->interrupt_num].ctrl;
        ep->hw_data_buf = &usbh_dpram->epx_data[64 * (ep->interrupt_num + 2)];
    } else {
        ep = &epx;
        ep->buffer_control = &usbh_dpram->epx_buf_ctrl;
        ep->endpoint_control = &usbh_dpram->epx_ctrl;
        ep->hw_data_buf = &usbh_dpram->epx_data[0];
    }

    return ep;
}

static void _hw_endpoint_init(struct hw_endpoint *ep, uint8_t dev_addr, uint8_t ep_addr,
                              uint16_t wMaxPacketSize, uint8_t transfer_type, uint8_t bmInterval) {
    assert(ep->endpoint_control);
    assert(ep->buffer_control);
    assert(ep->hw_data_buf);

    uint8_t const num = tu_edpt_number(ep_addr);
    tusb_dir_t const dir = tu_edpt_dir(ep_addr);

    ep->ep_addr = ep_addr;
    ep->dev_addr = dev_addr;
    ep->rx = (dir == TUSB_DIR_IN);
    ep->next_pid = (num == 0 ? 1u : 0u);
    ep->wMaxPacketSize = wMaxPacketSize;
    ep->transfer_type = transfer_type;

    uint dpram_offset = hw_data_offset(ep->hw_data_buf);
    assert(!(dpram_offset & 0b111111));

    uint32_t ep_reg = EP_CTRL_ENABLE_BITS
                      | EP_CTRL_INTERRUPT_PER_BUFFER
                      | (ep->transfer_type << EP_CTRL_BUFFER_TYPE_LSB)
                      | dpram_offset;
    if (bmInterval) {
        ep_reg |= (uint32_t)((bmInterval - 1) << EP_CTRL_HOST_INTERRUPT_INTERVAL_LSB);
    }
    *ep->endpoint_control = ep_reg;
    ep->configured = true;

    if (ep != &epx) {
        uint32_t reg = (uint32_t)(dev_addr | (num << USB_ADDR_ENDP1_ENDPOINT_LSB));

        if (dir == TUSB_DIR_OUT) {
            reg |= USB_ADDR_ENDP1_INTEP_DIR_BITS;
        }

        if (need_pre(dev_addr)) {
            reg |= USB_ADDR_ENDP1_INTEP_PREAMBLE_BITS;
        }
        usb_hw->int_ep_addr_ctrl[ep->interrupt_num] = reg;
        usb_hw_set->int_ep_ctrl = 1 << (ep->interrupt_num + 1);
    }
}

//--------------------------------------------------------------------+
// PIO-USB IRQ Handler (called from pio_usb library)
//--------------------------------------------------------------------+
static void __no_inline_not_in_flash_func(handle_endpoint_irq)(root_port_t *rport, xfer_result_t result,
                                                               volatile uint32_t *ep_reg) {
    (void)rport;
    const uint32_t ep_all = *ep_reg;

    for (uint8_t ep_idx = 0; ep_idx < PIO_USB_EP_POOL_CNT; ep_idx++) {
        uint32_t const mask = (1u << ep_idx);
        if (ep_all & mask) {
            endpoint_t *ep = PIO_USB_ENDPOINT(ep_idx);
            hcd_event_xfer_complete(ep->dev_addr, ep->ep_num, ep->actual_len, result, true);
        }
    }
    (*ep_reg) &= ~ep_all;
}

void __no_inline_not_in_flash_func(pio_usb_host_irq_handler)(uint8_t root_id) {
    uint8_t const tu_rhport = root_id + RHPORT_PIO_OFFSET;
    root_port_t *rport = PIO_USB_ROOT_PORT(root_id);
    uint32_t const ints = rport->ints;

    if (ints & PIO_USB_INTS_ENDPOINT_COMPLETE_BITS) {
        handle_endpoint_irq(rport, XFER_RESULT_SUCCESS, &rport->ep_complete);
    }

    if (ints & PIO_USB_INTS_ENDPOINT_STALLED_BITS) {
        handle_endpoint_irq(rport, XFER_RESULT_STALLED, &rport->ep_stalled);
    }

    if (ints & PIO_USB_INTS_ENDPOINT_ERROR_BITS) {
        handle_endpoint_irq(rport, XFER_RESULT_FAILED, &rport->ep_error);
    }

    if (ints & PIO_USB_INTS_CONNECT_BITS) {
        hcd_event_device_attach(tu_rhport, true);
    }

    if (ints & PIO_USB_INTS_DISCONNECT_BITS) {
        hcd_event_device_remove(tu_rhport, true);
    }

    rport->ints &= ~ints;
}

//--------------------------------------------------------------------+
// HCD API - Hybrid Implementation
//--------------------------------------------------------------------+

bool hcd_configure(uint8_t rhport, uint32_t cfg_id, const void *cfg_param) {
    if (IS_PIO_PORT(rhport)) {
        TU_VERIFY(cfg_id == TUH_CFGID_RPI_PIO_USB_CONFIGURATION);
        memcpy(&pio_host_cfg, cfg_param, sizeof(pio_usb_configuration_t));
        return true;
    }
    // Native USB doesn't need configuration
    return true;
}

bool hcd_init(uint8_t rhport, const tusb_rhport_init_t *rh_init) {
    (void)rh_init;

    if (IS_NATIVE_PORT(rhport)) {
        // Initialize native USB host
        rp2040_usb_init();
        usb_hw->pwr = USB_USB_PWR_VBUS_DETECT_BITS | USB_USB_PWR_VBUS_DETECT_OVERRIDE_EN_BITS;
        irq_remove_handler(USBCTRL_IRQ, hcd_rp2040_irq);
        irq_add_shared_handler(USBCTRL_IRQ, hcd_rp2040_irq, PICO_SHARED_IRQ_HANDLER_HIGHEST_ORDER_PRIORITY);
        memset(&ep_pool, 0, sizeof(ep_pool));
        usb_hw->main_ctrl = USB_MAIN_CTRL_CONTROLLER_EN_BITS | USB_MAIN_CTRL_HOST_NDEVICE_BITS;
        usb_hw->sie_ctrl = SIE_CTRL_BASE;
        usb_hw->inte = USB_INTE_BUFF_STATUS_BITS      |
                       USB_INTE_HOST_CONN_DIS_BITS    |
                       USB_INTE_HOST_RESUME_BITS      |
                       USB_INTE_STALL_BITS            |
                       USB_INTE_TRANS_COMPLETE_BITS   |
                       USB_INTE_ERROR_RX_TIMEOUT_BITS |
                       USB_INTE_ERROR_DATA_SEQ_BITS;
        return true;
    }

    if (IS_PIO_PORT(rhport) && !pio_usb_initialized) {
        // Initialize PIO-USB host (only once)
        pio_usb_host_init(&pio_host_cfg);
        pio_usb_initialized = true;
        return true;
    }

    return true;
}

bool hcd_deinit(uint8_t rhport) {
    if (IS_NATIVE_PORT(rhport)) {
        irq_remove_handler(USBCTRL_IRQ, hcd_rp2040_irq);
        reset_block(RESETS_RESET_USBCTRL_BITS);
        unreset_block_wait(RESETS_RESET_USBCTRL_BITS);
    }
    return true;
}

void hcd_int_handler(uint8_t rhport, bool in_isr) {
    (void)in_isr;
    if (IS_NATIVE_PORT(rhport)) {
        hcd_rp2040_irq();
    }
    // PIO-USB uses its own interrupt handler via pio_usb_host_irq_handler
}

void hcd_int_enable(uint8_t rhport) {
    if (IS_NATIVE_PORT(rhport)) {
        irq_set_enabled(USBCTRL_IRQ, true);
    }
    // PIO-USB interrupts are always enabled
}

void hcd_int_disable(uint8_t rhport) {
    if (IS_NATIVE_PORT(rhport)) {
        irq_set_enabled(USBCTRL_IRQ, false);
    }
}

uint32_t hcd_frame_number(uint8_t rhport) {
    if (IS_NATIVE_PORT(rhport)) {
        return usb_hw->sof_rd;
    }
    return pio_usb_host_get_frame_number();
}

//--------------------------------------------------------------------+
// Port API - Hybrid Implementation
//--------------------------------------------------------------------+

bool hcd_port_connect_status(uint8_t rhport) {
    if (IS_NATIVE_PORT(rhport)) {
        return usb_hw->sie_status & USB_SIE_STATUS_SPEED_BITS;
    }

    uint8_t const pio_rhport = RHPORT_PIO(rhport);
    root_port_t *root = PIO_USB_ROOT_PORT(pio_rhport);
    port_pin_status_t line_state = pio_usb_bus_get_line_state(root);
    return line_state != PORT_PIN_SE0;
}

void hcd_port_reset(uint8_t rhport) {
    if (IS_NATIVE_PORT(rhport)) {
        // Native USB: nothing to do
        return;
    }

    uint8_t const pio_rhport = RHPORT_PIO(rhport);
    pio_usb_host_port_reset_start(pio_rhport);
}

void hcd_port_reset_end(uint8_t rhport) {
    if (IS_NATIVE_PORT(rhport)) {
        return;
    }

    uint8_t const pio_rhport = RHPORT_PIO(rhport);
    pio_usb_host_port_reset_end(pio_rhport);
}

tusb_speed_t hcd_port_speed_get(uint8_t rhport) {
    if (IS_NATIVE_PORT(rhport)) {
        switch (dev_speed()) {
            case 1: return TUSB_SPEED_LOW;
            case 2: return TUSB_SPEED_FULL;
            default: return TUSB_SPEED_FULL;
        }
    }

    uint8_t const pio_rhport = RHPORT_PIO(rhport);
    return PIO_USB_ROOT_PORT(pio_rhport)->is_fullspeed ? TUSB_SPEED_FULL : TUSB_SPEED_LOW;
}

void hcd_device_close(uint8_t rhport, uint8_t dev_addr) {
    if (IS_NATIVE_PORT(rhport)) {
        if (dev_addr == 0) return;

        for (size_t i = 1; i < TU_ARRAY_SIZE(ep_pool); i++) {
            hw_endpoint_t *ep = &ep_pool[i];
            if (ep->dev_addr == dev_addr && ep->configured) {
                usb_hw_clear->int_ep_ctrl = (1 << (ep->interrupt_num + 1));
                usb_hw->int_ep_addr_ctrl[ep->interrupt_num] = 0;
                ep->configured = false;
                *ep->endpoint_control = 0;
                *ep->buffer_control = 0;
                hw_endpoint_reset_transfer(ep);
            }
        }
        return;
    }

    uint8_t const pio_rhport = RHPORT_PIO(rhport);
    pio_usb_host_close_device(pio_rhport, dev_addr);
}

//--------------------------------------------------------------------+
// Endpoint API - Hybrid Implementation
//--------------------------------------------------------------------+

bool hcd_edpt_open(uint8_t rhport, uint8_t dev_addr, tusb_desc_endpoint_t const *desc_ep) {
    if (IS_NATIVE_PORT(rhport)) {
        struct hw_endpoint *ep = _hw_endpoint_allocate(desc_ep->bmAttributes.xfer);
        TU_ASSERT(ep);
        _hw_endpoint_init(ep, dev_addr, desc_ep->bEndpointAddress,
                          tu_edpt_packet_size(desc_ep), desc_ep->bmAttributes.xfer, desc_ep->bInterval);
        return true;
    }

    hcd_devtree_info_t dev_tree;
    hcd_devtree_get_info(dev_addr, &dev_tree);
    bool const need_pre_token = (dev_tree.hub_addr && dev_tree.speed == TUSB_SPEED_LOW);

    uint8_t const pio_rhport = RHPORT_PIO(rhport);
    return pio_usb_host_endpoint_open(pio_rhport, dev_addr, (uint8_t const *)desc_ep, need_pre_token);
}

bool hcd_edpt_xfer(uint8_t rhport, uint8_t dev_addr, uint8_t ep_addr, uint8_t *buffer, uint16_t buflen) {
    if (IS_NATIVE_PORT(rhport)) {
        uint8_t const ep_num = tu_edpt_number(ep_addr);
        tusb_dir_t const ep_dir = tu_edpt_dir(ep_addr);
        struct hw_endpoint *ep = get_dev_ep(dev_addr, ep_addr);
        TU_ASSERT(ep);
        assert(!ep->active);

        if (ep_addr != ep->ep_addr) {
            assert(ep_num == 0);
            _hw_endpoint_init(ep, dev_addr, ep_addr, ep->wMaxPacketSize, ep->transfer_type, 0);
        }

        if (ep == &epx) {
            hw_endpoint_xfer_start(ep, buffer, buflen);
            usb_hw->dev_addr_ctrl = (uint32_t)(dev_addr | (ep_num << USB_ADDR_ENDP_ENDPOINT_LSB));

            uint32_t flags = USB_SIE_CTRL_START_TRANS_BITS | SIE_CTRL_BASE |
                             (ep_dir ? USB_SIE_CTRL_RECEIVE_DATA_BITS : USB_SIE_CTRL_SEND_DATA_BITS) |
                             (need_pre(dev_addr) ? USB_SIE_CTRL_PREAMBLE_EN_BITS : 0);
            usb_hw->sie_ctrl = flags & ~USB_SIE_CTRL_START_TRANS_BITS;
            busy_wait_at_least_cycles(12);
            usb_hw->sie_ctrl = flags;
        } else {
            hw_endpoint_xfer_start(ep, buffer, buflen);
        }
        return true;
    }

    uint8_t const pio_rhport = RHPORT_PIO(rhport);
    return pio_usb_host_endpoint_transfer(pio_rhport, dev_addr, ep_addr, buffer, buflen);
}

bool hcd_edpt_abort_xfer(uint8_t rhport, uint8_t dev_addr, uint8_t ep_addr) {
    if (IS_NATIVE_PORT(rhport)) {
        return false;  // Not implemented for native USB
    }

    uint8_t const pio_rhport = RHPORT_PIO(rhport);
    return pio_usb_host_endpoint_abort_transfer(pio_rhport, dev_addr, ep_addr);
}

bool hcd_setup_send(uint8_t rhport, uint8_t dev_addr, uint8_t const setup_packet[8]) {
    if (IS_NATIVE_PORT(rhport)) {
        for (uint8_t i = 0; i < 8; i++) {
            usbh_dpram->setup_packet[i] = setup_packet[i];
        }

        struct hw_endpoint *ep = _hw_endpoint_allocate(0);
        TU_ASSERT(ep);
        assert(!ep->active);

        _hw_endpoint_init(ep, dev_addr, 0x00, ep->wMaxPacketSize, 0, 0);
        assert(ep->configured);

        ep->remaining_len = 8;
        ep->active = true;

        usb_hw->dev_addr_ctrl = dev_addr;

        uint32_t const flags = SIE_CTRL_BASE | USB_SIE_CTRL_SEND_SETUP_BITS | USB_SIE_CTRL_START_TRANS_BITS |
                               (need_pre(dev_addr) ? USB_SIE_CTRL_PREAMBLE_EN_BITS : 0);
        usb_hw->sie_ctrl = flags & ~USB_SIE_CTRL_START_TRANS_BITS;
        busy_wait_at_least_cycles(12);
        usb_hw->sie_ctrl = flags;
        return true;
    }

    uint8_t const pio_rhport = RHPORT_PIO(rhport);
    return pio_usb_host_send_setup(pio_rhport, dev_addr, setup_packet);
}

bool hcd_edpt_clear_stall(uint8_t rhport, uint8_t dev_addr, uint8_t ep_addr) {
    (void)rhport;
    (void)dev_addr;
    (void)ep_addr;
    return true;
}

#endif // CFG_TUH_RPI_HYBRID_USB

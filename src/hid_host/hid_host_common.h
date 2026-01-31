#ifndef _TUSB_HID_HOST_COMMON_H_
#define _TUSB_HID_HOST_COMMON_H_

#include "tusb.h"
#include "hid_rip.h"

#ifdef __cplusplus
 extern "C" {
#endif

typedef union TU_ATTR_PACKED
{
  uint8_t byte;
  struct TU_ATTR_PACKED
  {
      bool data_const          : 1;
      bool array_variable      : 1;
      bool absolute_relative   : 1;
      bool nowrap_wrap         : 1;
      bool linear_nonlinear    : 1;
      bool prefered_noprefered : 1;
      bool nonull_null         : 1;
  };
} tusb_hid_ri_intput_flags_t;

typedef struct {
  union TU_ATTR_PACKED
  {
    uint8_t byte;
    struct TU_ATTR_PACKED
    {
        bool is_signed    : 1;
    };
  } flags;
  uint16_t start;
  uint16_t length;
  int32_t logical_min;
  int32_t logical_max;
  int32_t threshold_min;
  int32_t threshold_max;
} tusb_hid_simple_axis_t;

typedef struct {
  uint16_t start;
  uint16_t length;
} tusb_hid_simple_buttons_t;

// Intermediate data structure used while parsing joystick HID report descriptors
typedef struct {
  uint32_t report_size;
  uint32_t report_count;
  int32_t logical_min;
  int32_t logical_max;
  uint16_t usage_page;
  uint16_t usage_min;
  uint16_t usage_max;
  uint8_t report_id;
  tusb_hid_ri_intput_flags_t input_flags;
  bool usage_is_range;
} tuh_hid_simple_input_data_t;

void tuh_hid_simple_init_axis(
  tusb_hid_simple_axis_t* simple_axis
);

// Fetch some data from the HID parser
//
// The data fetched here may be relevant to multiple usage items
//
// returns false if obviously not of interest
bool tuh_hid_get_simple_input_data(
  tuh_hid_rip_state_t *pstate,        // The current HID report parser state
  const uint8_t* ri_input,            // Pointer to the input item we have arrived at
  tuh_hid_simple_input_data_t* data   // Data structure to complete
);

void tuh_hid_process_simple_axis(
  tuh_hid_simple_input_data_t* jdata,
  uint32_t bitpos,
  tusb_hid_simple_axis_t* simple_axis
);

#ifdef __cplusplus
}
#endif

#endif /* _TUSB_HID_HOST_COMMON_H_ */

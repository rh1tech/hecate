#include "hid_host_common.h"

void tuh_hid_simple_init_axis(
  tusb_hid_simple_axis_t* simple_axis)
{
  simple_axis->start = 0;
  simple_axis->length = 0;
  simple_axis->flags.is_signed = true;
  simple_axis->logical_min = -1;
  simple_axis->logical_max = 1;
  simple_axis->threshold_min = -1;
  simple_axis->threshold_max = 1;
}

// Fetch some data from the HID parser
//
// The data fetched here may be relevant to multiple usage items
//
// returns false if obviously not of interest
bool tuh_hid_get_simple_input_data(
  tuh_hid_rip_state_t *pstate,        // The current HID report parser state
  const uint8_t* ri_input,            // Pointer to the input item we have arrived at
  tuh_hid_simple_input_data_t* data)  // Data structure to complete
{
  const uint8_t* ri_report_size = tuh_hid_rip_global(pstate, RI_GLOBAL_REPORT_SIZE);
  const uint8_t* ri_report_count = tuh_hid_rip_global(pstate, RI_GLOBAL_REPORT_COUNT);
  const uint8_t* ri_report_id = tuh_hid_rip_global(pstate, RI_GLOBAL_REPORT_ID);
  const uint8_t* ri_logical_min = tuh_hid_rip_global(pstate, RI_GLOBAL_LOGICAL_MIN);
  const uint8_t* ri_logical_max = tuh_hid_rip_global(pstate, RI_GLOBAL_LOGICAL_MAX);
  const uint8_t* ri_usage_page = tuh_hid_rip_global(pstate, RI_GLOBAL_USAGE_PAGE);
  const uint8_t* ri_usage_min = tuh_hid_rip_local(pstate, RI_LOCAL_USAGE_MIN);
  const uint8_t* ri_usage_max = tuh_hid_rip_local(pstate, RI_LOCAL_USAGE_MAX);

  // We need to know how the size of the data
  if (ri_report_size == NULL || ri_report_count == NULL || ri_usage_page == NULL) return false;

  data->report_size = tuh_hid_ri_short_udata32(ri_report_size);
  data->report_count = tuh_hid_ri_short_udata32(ri_report_count);
  data->report_id = ri_report_id ? tuh_hid_ri_short_udata8(ri_report_id) : 0;
  data->logical_min = ri_logical_min ? tuh_hid_ri_short_data32(ri_logical_min) : 0;
  data->logical_max = ri_logical_max ? tuh_hid_ri_short_data32(ri_logical_max) : 0;
  data->input_flags.byte = tuh_hid_ri_short_udata8(ri_input);
  data->usage_page = (uint16_t)tuh_hid_ri_short_udata32(ri_usage_page);
  data->usage_min = ri_usage_min ? (uint16_t)tuh_hid_ri_short_udata32(ri_usage_min) : 0;
  data->usage_max = ri_usage_max ? (uint16_t)tuh_hid_ri_short_udata32(ri_usage_max) : 0;
  data->usage_is_range = (ri_usage_min != NULL) && (ri_usage_max != NULL);

  return true;
}

void tuh_hid_process_simple_axis(
  tuh_hid_simple_input_data_t* jdata,
  uint32_t bitpos,
  tusb_hid_simple_axis_t* simple_axis)
{
  simple_axis->start = (uint16_t)bitpos;
  simple_axis->length = (uint16_t)jdata->report_size;
  simple_axis->flags.is_signed = jdata->logical_min < 0;
  simple_axis->logical_min = jdata->logical_min;
  simple_axis->logical_max = jdata->logical_max;

  if  (simple_axis->flags.is_signed) {
    simple_axis->threshold_min = jdata->logical_min / 2;
    simple_axis->threshold_max = jdata->logical_max / 2;
  }
  else {
    const int quater = (jdata->logical_max-jdata->logical_min) / 4;
    simple_axis->threshold_min = jdata->logical_min+quater;
    simple_axis->threshold_max = jdata->logical_max-quater;
  }
}

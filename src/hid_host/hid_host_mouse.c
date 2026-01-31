
#include "hid_host_mouse.h"
#include "hid_host_utils.h"
#include "bsp/board.h"

static tusb_hid_simple_mouse_t hid_simple_mice[HID_MAX_MICE];

static bool tuh_hid_mouse_check_usage(uint32_t eusage)
{
  // Check outer usage
  switch(eusage) {
    case HID_RIP_EUSAGE(HID_USAGE_PAGE_DESKTOP, HID_USAGE_DESKTOP_MOUSE): return true;
    default: return false;
  }
}

tusb_hid_simple_mouse_t* tuh_hid_get_simple_mouse(uint8_t dev_addr, uint8_t instance, uint8_t report_id)
{
  tusb_hid_simple_mouse_key_t key;
  key.elements.dev_addr = dev_addr;
  key.elements.instance = instance;
  key.elements.report_id = report_id;
  key.elements.in_use = 1;
  uint32_t combined = key.combined;
  
  for(uint8_t i = 0; i < HID_MAX_MICE; ++i) {
    tusb_hid_simple_mouse_t* simple_mouse = &hid_simple_mice[i];
    if (simple_mouse->key.combined == combined) return simple_mouse;
  }
  return NULL;
}

void tuh_hid_free_simple_mice(void) {
  for(uint8_t i = 0; i < HID_MAX_MICE; ++i) {
    hid_simple_mice[i].key.elements.in_use = false;
  } 
}

void tuh_hid_free_simple_mice_for_instance(uint8_t dev_addr, uint8_t instance) {
  for(uint8_t i = 0; i < HID_MAX_MICE; ++i) {
    tusb_hid_simple_mouse_t* simple_mouse = &hid_simple_mice[i];
    if (simple_mouse->key.elements.dev_addr == dev_addr && simple_mouse->key.elements.instance == instance) simple_mouse->key.elements.in_use = 0;
  }
}

tusb_hid_simple_mouse_t* tuh_hid_allocate_simple_mouse(uint8_t dev_addr, uint8_t instance, uint8_t report_id) {
  for(uint8_t i = 0; i < HID_MAX_MICE; ++i) {
    tusb_hid_simple_mouse_t* simple_mouse = &hid_simple_mice[i];
    if (!simple_mouse->key.elements.in_use) {
      tu_memclr(simple_mouse, sizeof(tusb_hid_simple_mouse_t));
      simple_mouse->key.elements.in_use = 1;;
      simple_mouse->key.elements.instance = instance;
      simple_mouse->key.elements.report_id = report_id;
      simple_mouse->key.elements.dev_addr = dev_addr;
      tuh_hid_simple_init_axis(&simple_mouse->axis_x1);
      tuh_hid_simple_init_axis(&simple_mouse->axis_y1);
      tuh_hid_simple_init_axis(&simple_mouse->axis_w1);
      return simple_mouse;
    }
  }
  return NULL;
}

// get or create
tusb_hid_simple_mouse_t* tuh_hid_obtain_simple_mouse(uint8_t dev_addr, uint8_t instance, uint8_t report_id) {
  tusb_hid_simple_mouse_t* data = tuh_hid_get_simple_mouse(dev_addr, instance, report_id);
  return data ? data : tuh_hid_allocate_simple_mouse(dev_addr, instance, report_id);
}

void tuh_hid_mouse_process_usages(
  tuh_hid_rip_state_t *pstate,
  tuh_hid_simple_input_data_t* jdata,
  uint32_t bitpos,
  uint8_t dev_addr,
  uint8_t instance)
{
  if (jdata->input_flags.data_const) return;
  
  // If there are no specific usages look for a range
  // TODO What is the correct behaviour if there are both?
  if (pstate->usage_count == 0 && !jdata->usage_is_range) {
    printf("no usage - skipping bits %lu \n", jdata->report_size * jdata->report_count);
    return;
  }

  tusb_hid_simple_mouse_t* simple_mouse = tuh_hid_obtain_simple_mouse(dev_addr, instance, jdata->report_id);
  
  if (simple_mouse == NULL) {
    printf("Failed to allocate mouse for HID dev_addr %d, instance %d, report ID %d\n", dev_addr, instance, jdata->report_id);
    return;
  }

  // Update the report length in bytes
  simple_mouse->report_length = (uint8_t)((bitpos + (jdata->report_size * jdata->report_count) + 7) >> 3);

  // Naive, assumes buttons are defined in a range
  if (jdata->usage_is_range) {
    if (jdata->usage_page == HID_USAGE_PAGE_BUTTON) {
      tusb_hid_simple_buttons_t* simple_buttons = &simple_mouse->buttons;
      simple_buttons->start = (uint16_t)bitpos;
      simple_buttons->length = (uint16_t)jdata->report_count;
      return;
    }
  }

  for (uint8_t i = 0; i < jdata->report_count; ++i) {
    uint32_t eusage = pstate->usages[i < pstate->usage_count ? i : pstate->usage_count - 1];
    switch (eusage) {
      // Seems to be common usage for gamepads.
      // Probably needs a lot more thought...
      case HID_RIP_EUSAGE(HID_USAGE_PAGE_DESKTOP, HID_USAGE_DESKTOP_X):    
        tuh_hid_process_simple_axis(jdata, bitpos, &simple_mouse->axis_x1);
        break;
      case HID_RIP_EUSAGE(HID_USAGE_PAGE_DESKTOP, HID_USAGE_DESKTOP_Y):
        tuh_hid_process_simple_axis(jdata, bitpos, &simple_mouse->axis_y1);
        break;
      case HID_RIP_EUSAGE(HID_USAGE_PAGE_DESKTOP, HID_USAGE_DESKTOP_WHEEL):
        tuh_hid_process_simple_axis(jdata, bitpos, &simple_mouse->axis_w1);
        break;
      default: break;
    }
    bitpos += jdata->report_size;
  }
}

// TODO very similar to mouse
void tuh_hid_mouse_parse_report_descriptor(uint8_t const* desc_report, uint16_t desc_len, uint8_t dev_addr, uint8_t instance) 
{
  uint32_t eusage = 0;
  tuh_hid_rip_state_t pstate;
  tuh_hid_rip_init_state(&pstate, desc_report, desc_len);
  const uint8_t *ri;
  uint32_t bitpos = 0;
  while((ri = tuh_hid_rip_next_short_item(&pstate)) != NULL)
  {
    uint8_t const type_and_tag = tuh_hid_ri_short_type_and_tag(ri);

    switch(type_and_tag)
    {
      case HID_RI_TYPE_AND_TAG(RI_TYPE_MAIN, RI_MAIN_INPUT): {
        if (tuh_hid_mouse_check_usage(eusage)) {
          tuh_hid_simple_input_data_t mouse_data;
          if(tuh_hid_get_simple_input_data(&pstate, ri, &mouse_data)) {
            tuh_hid_mouse_process_usages(&pstate, &mouse_data, bitpos, dev_addr, instance);
            bitpos += mouse_data.report_size * mouse_data.report_count;
          }
        }
        break;
      }
      case HID_RI_TYPE_AND_TAG(RI_TYPE_LOCAL, RI_LOCAL_USAGE): {
        if (pstate.collections_count == 0) {
          eusage = pstate.usages[pstate.usage_count - 1];
        }
        break;
      }
      case HID_RI_TYPE_AND_TAG(RI_TYPE_GLOBAL, RI_GLOBAL_REPORT_ID): {
        bitpos = 0;
        break;
      }
      default: break;
    }
  }
}

int32_t tuh_hid_simple_mouse_get_axis_value(tusb_hid_simple_axis_t* simple_axis, const uint8_t* report) 
{
  return tuh_hid_report_i32(report, simple_axis->start, simple_axis->length, simple_axis->flags.is_signed);
}

void tusb_hid_simple_mouse_process_report(tusb_hid_simple_mouse_t* simple_mouse, const uint8_t* report, uint8_t report_length)
 {   
  if (simple_mouse->report_length > report_length) {
    TU_LOG1("HID mouse report too small\r\n");
    return;
  }
  tusb_hid_simple_mouse_values_t* values = &simple_mouse->values;
  values->x1 = tuh_hid_simple_mouse_get_axis_value(&simple_mouse->axis_x1, report);
  values->y1 = tuh_hid_simple_mouse_get_axis_value(&simple_mouse->axis_y1, report);
  values->w1 = tuh_hid_simple_mouse_get_axis_value(&simple_mouse->axis_w1, report);
  values->buttons = tuh_hid_report_bits_u32(report, simple_mouse->buttons.start, simple_mouse->buttons.length);
  simple_mouse->has_values = true;
  simple_mouse->updated = board_millis();

   // tusb_hid_print_simple_mouse_report(simple_mouse);
}

void tusb_hid_print_simple_mouse_report(tusb_hid_simple_mouse_t* simple_mouse)
{
  if (simple_mouse->has_values) {  
    printf("dev_addr=%3d, instance=%3d, report_id=%3d, x1=%4ld, y1=%4ld, w1=%4ld, buttons=%04lX\n",  
      simple_mouse->key.elements.dev_addr,
      simple_mouse->key.elements.instance,
      simple_mouse->key.elements.report_id,
      simple_mouse->values.x1,
      simple_mouse->values.y1,
      simple_mouse->values.w1,
      simple_mouse->values.buttons);    
  }
}

uint8_t tuh_hid_get_simple_mice(tusb_hid_simple_mouse_t** simple_mice, uint8_t max_simple_mice)
{
  uint8_t j = 0;
  for(uint8_t i = 0; i < HID_MAX_MICE && j < max_simple_mice; ++i) {
    tusb_hid_simple_mouse_t* simple_mouse = &hid_simple_mice[i];
    if (simple_mouse->key.elements.in_use) {
      simple_mice[j++] = simple_mouse;
    }
  }  
  return j;
}

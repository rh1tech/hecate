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

#include "hid_host_joy.h"
#include "hid_host_utils.h"
#include "bsp/board.h"

static tusb_hid_simple_joystick_t hid_simple_joysticks[HID_MAX_JOYSTICKS];

static bool tuh_hid_joystick_check_usage(uint32_t eusage)
{
  // Check outer usage
  switch(eusage) {
    case HID_RIP_EUSAGE(HID_USAGE_PAGE_DESKTOP, HID_USAGE_DESKTOP_JOYSTICK): return true;
    case HID_RIP_EUSAGE(HID_USAGE_PAGE_DESKTOP, HID_USAGE_DESKTOP_GAMEPAD): return true; // TODO not sure about this one
    default: return false;
  }
}

tusb_hid_simple_joystick_t* tuh_hid_get_simple_joystick(uint8_t dev_addr, uint8_t instance, uint8_t report_id)
{
  tusb_hid_simple_joystick_key_t key;
  key.elements.dev_addr = dev_addr;
  key.elements.instance = instance;
  key.elements.report_id = report_id;
  key.elements.in_use = 1;
  uint32_t combined = key.combined;
  
  for(uint8_t i = 0; i < HID_MAX_JOYSTICKS; ++i) {
    tusb_hid_simple_joystick_t* simple_joystick = &hid_simple_joysticks[i];
    if (simple_joystick->key.combined == combined) return simple_joystick;
  }
  return NULL;
}

void tuh_hid_free_simple_joysticks(void) {
  for(uint8_t i = 0; i < HID_MAX_JOYSTICKS; ++i) {
    hid_simple_joysticks[i].key.elements.in_use = false;
  } 
}

void tuh_hid_free_simple_joysticks_for_instance(uint8_t dev_addr, uint8_t instance) {
  for(uint8_t i = 0; i < HID_MAX_JOYSTICKS; ++i) {
    tusb_hid_simple_joystick_t* simple_joystick = &hid_simple_joysticks[i];
    if (simple_joystick->key.elements.dev_addr == dev_addr && simple_joystick->key.elements.instance == instance) simple_joystick->key.elements.in_use = 0;
  }
}

tusb_hid_simple_joystick_t* tuh_hid_allocate_simple_joystick(uint8_t dev_addr, uint8_t instance, uint8_t report_id) {
  for(uint8_t i = 0; i < HID_MAX_JOYSTICKS; ++i) {
    tusb_hid_simple_joystick_t* simple_joystick = &hid_simple_joysticks[i];
    if (!simple_joystick->key.elements.in_use) {
      tu_memclr(simple_joystick, sizeof(tusb_hid_simple_joystick_t));
      simple_joystick->key.elements.in_use = 1;
      simple_joystick->key.elements.instance = instance;
      simple_joystick->key.elements.report_id = report_id;
      simple_joystick->key.elements.dev_addr = dev_addr;
      tuh_hid_simple_init_axis(&simple_joystick->axis_x1);
      tuh_hid_simple_init_axis(&simple_joystick->axis_x2);
      tuh_hid_simple_init_axis(&simple_joystick->axis_y1);
      tuh_hid_simple_init_axis(&simple_joystick->axis_y2);
      tuh_hid_simple_init_axis(&simple_joystick->hat);
      return simple_joystick;
    }
  }
  return NULL;
}

// get or create
tusb_hid_simple_joystick_t* tuh_hid_obtain_simple_joystick(uint8_t dev_addr, uint8_t instance, uint8_t report_id) {
  tusb_hid_simple_joystick_t* jdata = tuh_hid_get_simple_joystick(dev_addr, instance, report_id);
  return jdata ? jdata : tuh_hid_allocate_simple_joystick(dev_addr, instance, report_id);
}

void tuh_hid_joystick_process_usages(
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

  tusb_hid_simple_joystick_t* simple_joystick = tuh_hid_obtain_simple_joystick(dev_addr, instance, jdata->report_id);
  
  if (simple_joystick == NULL) {
    printf("Failed to allocate joystick for HID dev_addr %d, instance %d, report ID %d\n", dev_addr, instance, jdata->report_id);
    return;
  }

  // Update the report length in bytes
  simple_joystick->report_length = (uint8_t)((bitpos + (jdata->report_size * jdata->report_count) + 7) >> 3);

  // Naive, assumes buttons are defined in a range
  if (jdata->usage_is_range) {
    if (jdata->usage_page == HID_USAGE_PAGE_BUTTON) {
      tusb_hid_simple_buttons_t* simple_buttons = &simple_joystick->buttons;
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
        tuh_hid_process_simple_axis(jdata, bitpos, &simple_joystick->axis_x1);
        break;
      case HID_RIP_EUSAGE(HID_USAGE_PAGE_DESKTOP, HID_USAGE_DESKTOP_Y):
        tuh_hid_process_simple_axis(jdata, bitpos, &simple_joystick->axis_y1);
        break;
      case HID_RIP_EUSAGE(HID_USAGE_PAGE_DESKTOP, HID_USAGE_DESKTOP_Z):
        tuh_hid_process_simple_axis(jdata, bitpos, &simple_joystick->axis_x2);
        break;
      case HID_RIP_EUSAGE(HID_USAGE_PAGE_DESKTOP, HID_USAGE_DESKTOP_RZ):
        tuh_hid_process_simple_axis(jdata, bitpos, &simple_joystick->axis_y2);
        break;      
      case HID_RIP_EUSAGE(HID_USAGE_PAGE_DESKTOP, HID_USAGE_DESKTOP_HAT_SWITCH):
        tuh_hid_process_simple_axis(jdata, bitpos, &simple_joystick->hat);
        break;
      default: break;
    }
    bitpos += jdata->report_size;
  }
}

void tuh_hid_joystick_parse_report_descriptor(uint8_t const* desc_report, uint16_t desc_len, uint8_t dev_addr, uint8_t instance) 
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
        if (tuh_hid_joystick_check_usage(eusage)) {
          tuh_hid_simple_input_data_t joystick_data;
          if(tuh_hid_get_simple_input_data(&pstate, ri, &joystick_data)) {
            tuh_hid_joystick_process_usages(&pstate, &joystick_data, bitpos, dev_addr, instance);
            bitpos += joystick_data.report_size * joystick_data.report_count;
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

int32_t tuh_hid_simple_joystick_get_axis_value(tusb_hid_simple_axis_t* simple_axis, const uint8_t* report) 
{
  return tuh_hid_report_i32(report, simple_axis->start, simple_axis->length, simple_axis->flags.is_signed);
}

void tusb_hid_simple_joystick_process_report(tusb_hid_simple_joystick_t* simple_joystick, const uint8_t* report, uint8_t report_length)
 {   
  if (simple_joystick->report_length > report_length) {
    TU_LOG1("HID joystick report too small\r\n");
    return;
  }
  tusb_hid_simple_joystick_values_t* values = &simple_joystick->values;
  values->x1 = tuh_hid_simple_joystick_get_axis_value(&simple_joystick->axis_x1, report);
  values->y1 = tuh_hid_simple_joystick_get_axis_value(&simple_joystick->axis_y1, report);
  values->x2 = tuh_hid_simple_joystick_get_axis_value(&simple_joystick->axis_x2, report);
  values->y2 = tuh_hid_simple_joystick_get_axis_value(&simple_joystick->axis_y2, report);
  values->hat = tuh_hid_simple_joystick_get_axis_value(&simple_joystick->hat, report);
  values->buttons = tuh_hid_report_bits_u32(report, simple_joystick->buttons.start, simple_joystick->buttons.length);
  simple_joystick->has_values = true;
  simple_joystick->updated = board_millis();

  // tusb_hid_print_simple_joystick_report(simple_joystick);
}

void tusb_hid_print_simple_joystick_report(tusb_hid_simple_joystick_t* simple_joystick)
{
  if (simple_joystick->has_values) {  
    printf("dev_addr=%3d, instance=%3d, report_id=%3d, x1=%4ld, y1=%4ld, x2=%4ld, y2=%4ld, hat=%01lX, buttons=%04lX\n",  
      simple_joystick->key.elements.dev_addr,
      simple_joystick->key.elements.instance,
      simple_joystick->key.elements.report_id,
      simple_joystick->values.x1,
      simple_joystick->values.y1,
      simple_joystick->values.x2,
      simple_joystick->values.y2,
      simple_joystick->values.hat,
      simple_joystick->values.buttons);    
  }
}

uint8_t tuh_hid_get_simple_joysticks(tusb_hid_simple_joystick_t** simple_joysticks, uint8_t max_simple_joysticks)
{
  uint8_t j = 0;
  for(uint8_t i = 0; i < HID_MAX_JOYSTICKS && j < max_simple_joysticks; ++i) {
    tusb_hid_simple_joystick_t* simple_joystick = &hid_simple_joysticks[i];
    if (simple_joystick->key.elements.in_use) {
      simple_joysticks[j++] = simple_joystick;
    }
  }  
  return j;
}


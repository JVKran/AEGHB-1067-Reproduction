
#pragma once
#include "tusb.h"
#include "tusb_config_uac.h"
#include "class/audio/audio.h"
#include "uac_config.h"
#include "uac_descriptors.h"

// #define ALT_COUNT 1

enum {
 ITF_NUM_MSC = 0,
 ITF_NUM_CDC,
 ITF_NUM_CDC_DATA,
//  ITF_NUM_DFU_MODE,
//  ITF_NUM_HID,
  ITF_NUM_AUDIO_CONTROL,
  ITF_NUM_AUDIO_STREAMING_SPK,
  ITF_NUM_TOTAL,
};


enum {
  EPNUM_CTRL_OUT = 0x00,
  EPNUM_CTRL_IN = 0x80,
  EPNUM_MSC_OUT = 0x01,
  EPNUM_MSC_IN = 0x81,
  EPNUM_CDC_OUT = 0x02,
  EPNUM_CDC_IN = 0x82,
  EPNUM_CDC_NOTIF = 0x83,
  EPNUM_AUDIO_OUT = 0x04,
  EPNUM_AUDIO_FB = 0x84,
};

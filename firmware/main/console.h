#pragma once
// Serial debug console (USB Serial/JTAG): tone, stop, ring, snooze, alarm,
// max_gain, heap, wifi, time, tz.

#include "esp_err.h"

namespace console {

esp_err_t init();

}  // namespace console

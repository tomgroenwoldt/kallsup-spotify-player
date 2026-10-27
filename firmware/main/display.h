#pragma once
// 0.96" SSD1306 OLED (128 x 64, I2C on GPIO 11/12) in the front of the base.
//
//   ┌────────────────────────┐
//   │ 07:42             ▂▄▆█ │  clock, Wi-Fi signal (yellow band, see BAND_Y)
//   │ IP 192.168.178.23      │  or the setup AP's name and password
//   │ WLAN MyNetwork         │
//   │ Wecker Mo 07:00        │  next alarm, ringing, snoozed, or no time sync
//   │ ────────────────────── │
//   │ ▶ Track title          │  long lines scroll
//   │   Artist               │
//   └────────────────────────┘
//
// After a while without interaction (30 s by default) a face replaces it: robot
// eyes that blink and look around, happy while music plays, asleep at night.
// Delay, night hours, brightness, the face and the display itself can be
// switched in the settings. Button
// presses, saved settings and a new track count as interaction. The info screen
// stays while the alarm rings or is snoozed, and without Wi-Fi or time sync.
//
// Text is German like the web page. Without a display on the bus init() logs a
// warning and returns ESP_OK, so the clock works without one.

#include "esp_err.h"

namespace display {

esp_err_t init();

// Shows the info screen again and restarts the face delay.
void wake();

}  // namespace display

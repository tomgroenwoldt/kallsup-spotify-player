#pragma once
// Onboard RGB LED showing the device state (kept dim, it's a bedroom):
//   ringing          warm white, breathing
//   snoozed          dim blue, slow pulse
//   error            red, blinking: no Wi-Fi, or no time sync (alarms can't fire)
//   warning          dim yellow: Web API set up but Spotify not logged in
//   idle, all good   very dim green

#include "esp_err.h"

namespace status_led {

esp_err_t init();

}  // namespace status_led

#pragma once
// Snooze / Stop buttons: GPIO 9 and 10, active LOW with internal pull-ups.
// Unconnected pins read as released.

#include "esp_err.h"

namespace buttons {

enum class Button { SNOOZE, STOP };

using Handler = void (*)(Button button);

// Polls both buttons every 10 ms and calls `on_press` once per debounced press.
esp_err_t init(Handler on_press);

}  // namespace buttons

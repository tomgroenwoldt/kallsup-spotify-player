#pragma once
// Snooze / Stop buttons: GPIO 9 and 10, active LOW with internal pull-ups.
// Unconnected pins read as released.

#include "esp_err.h"

namespace buttons {

enum class Button { SNOOZE, STOP };

// `clicks` presses in a row, each released within CLICK_GAP_MS of the next.
using Handler = void (*)(Button button, int clicks);

// Polls both buttons every 10 ms and calls `on_click` once a click series ends,
// CLICK_GAP_MS after its last release.
esp_err_t init(Handler on_click);

constexpr int CLICK_GAP_MS = 350;

}  // namespace buttons

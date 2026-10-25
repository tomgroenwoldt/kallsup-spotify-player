#pragma once
// Snooze / Stop buttons: GPIO 9 and 10, active LOW with internal pull-ups.
// Unconnected pins read as released.

#include "esp_err.h"

namespace buttons {

enum class Button { SNOOZE, STOP };

enum class Event {
    CLICKS,  // the series ended: released, then no press for CLICK_GAP_MS
    HOLD,    // the series' last press has been held for HOLD_MS
    REPEAT,  // still held, every REPEAT_MS after HOLD; release ends the series
};

// `clicks` counts the presses in the series, including a held one.
using Handler = void (*)(Button button, Event event, int clicks);

// Polls both buttons every 10 ms and reports click series and holds.
esp_err_t init(Handler on_event);

constexpr int CLICK_GAP_MS = 350;
constexpr int HOLD_MS = 400;
constexpr int REPEAT_MS = 200;

}  // namespace buttons

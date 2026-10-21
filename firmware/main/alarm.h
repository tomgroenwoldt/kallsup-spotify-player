#pragma once
// Alarm scheduler + state machine (section 4 of PLAN.md).
//
//   IDLE -> PREPARING (90 s before) -> RINGING_SPOTIFY | RINGING_FALLBACK
//        -> SNOOZED -> ... -> IDLE
//
// Rings with the Spotify track if possible, else (or if no Spotify audio
// arrives within 15 s) with a locally generated beep that needs no network.
// Without a clock synced since boot the alarm never guesses: it stays idle and
// reports time_ok() == false.

#include <ctime>

#include "esp_err.h"
#include "settings.h"

namespace alarm_clock {

enum class State { IDLE, PREPARING, RINGING_SPOTIFY, RINGING_FALLBACK, SNOOZED };

esp_err_t init();

// Ring now, as if an alarm time was reached.
void ring_now();
// Ring now with the fallback beep, skipping Spotify.
void ring_fallback();
// While ringing: silence and ring again after snooze_min.
void snooze();
// Ends a ringing or snoozed alarm, and stops any other audio.
void stop();

State state();
const char *state_name(State state);
bool ringing();
// When a snoozed alarm rings again; 0 if not snoozed.
time_t snoozed_until();
// False until the clock has been synced since boot; alarms can't fire then.
bool time_ok();

// Next time any enabled alarm in `cfg` fires strictly after `now`, in local
// time; 0 if none is enabled.
time_t next_alarm(const settings::Config &cfg, time_t now);

}  // namespace alarm_clock

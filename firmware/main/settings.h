#pragma once
// NVS-backed configuration (section 2.4 of PLAN.md).
//
// The whole configuration is one Config value: get() returns a copy, save()
// validates and stores a modified copy, then notifies change listeners.

#include <array>
#include <functional>
#include <string>

#include "esp_err.h"

namespace settings {

constexpr size_t MAX_ALARMS = 4;
constexpr const char *DEFAULT_TIME_ZONE = "CET-1CEST,M3.5.0,M10.5.0/3";  // Europe/Berlin

struct Alarm {
    bool enabled = false;
    int hour = 7;
    int minute = 0;
    int weekdays = 0x3E;  // bit n = tm_wday n (0 = Sunday); default Mon–Fri
};

struct Config {
    // Wi-Fi
    std::string wifi_ssid;
    std::string wifi_pass;  // secret
    // Time
    std::string time_zone = DEFAULT_TIME_ZONE;
    // Alarms
    std::array<Alarm, MAX_ALARMS> alarms{};
    // Track
    std::string track_uri;  // spotify:track:<22 chars>
    // Volume
    float max_gain = 0.6f;
    int fade_in_s = 30;
    // Behaviour
    int snooze_min = 9;
    int auto_stop_min = 30;
    bool repeat_track = true;
    // Spotify Web API
    std::string spotify_client_id;
    std::string spotify_client_secret;  // secret
    std::string spotify_refresh_token;  // secret
    // Device
    std::string device_name = "Kallsup Alarm";
};

using Listener = std::function<void(const Config &old_cfg, const Config &new_cfg)>;

// Loads the configuration from NVS; missing keys keep their defaults.
esp_err_t init();

Config get();

// Validates and stores `cfg`, then calls the listeners. On invalid input
// returns ESP_ERR_INVALID_ARG and sets `error` to a readable reason.
esp_err_t save(const Config &cfg, std::string *error = nullptr);

// Called after every successful save, in the saving task's context.
void on_change(Listener listener);

}  // namespace settings

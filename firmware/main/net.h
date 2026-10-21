#pragma once
// Wi-Fi (STA + fallback AP), SNTP, mDNS.
//
// Connects as a station with the credentials from settings. If none are stored,
// or there is no connection for 60 s, an open setup AP is started next to the
// station; it stops once the station gets an IP.

#include <string>

#include "esp_err.h"

namespace net {

constexpr const char *HOSTNAME = "kallsup-alarm";  // kallsup-alarm.local
constexpr const char *AP_SSID = "Kallsup-Setup";

struct Status {
    bool sta_connected = false;
    bool ap_active = false;
    std::string ssid;
    std::string ip;
    int rssi = 0;
};

esp_err_t init();

// Reconnects with the Wi-Fi credentials currently stored in settings.
// Saving new credentials, time zone or device name applies them automatically.
void reconnect();

// Applies the POSIX TZ string stored in settings.
void apply_time_zone();

Status status();

// True once SNTP has set the clock since boot.
bool time_synced();

}  // namespace net

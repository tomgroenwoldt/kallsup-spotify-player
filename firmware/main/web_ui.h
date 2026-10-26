#pragma once
// HTTP config page + JSON API on port 80.
//
//   GET  /                     the single-page UI (www/index.html)
//   GET  /anleitung            the printable German guide
//   GET  /api/wifi/scan        nearby networks; only while the setup AP is on
//   GET  /api/settings         all settings; secrets only as *_set booleans
//   POST /api/settings         partial update; for secrets, a non-empty string
//                              sets, null clears, absent/empty keeps
//   GET  /api/status           time, Wi-Fi, Spotify login, next alarm
//   POST /api/test/alarm       start the alarm now
//   POST /api/test/fallback    play the fallback beep
//   POST /api/test/snooze      snooze a ringing alarm
//   POST /api/test/stop        stop the alarm / playback
//   POST /api/ota              firmware update: the raw .bin as
//                              application/octet-stream; restarts on success
//
// POSTs must send Content-Type: application/json. Browsers then need a CORS
// preflight for cross-site requests, which this server never answers, so other
// websites can't change settings through a visitor's browser.
//
// Every route also checks the Host header: only IP literals and this device's
// own names (kallsup-alarm, kallsup-alarm.local, ...) are served. That stops DNS
// rebinding, where a website points its own name at this device to get around
// CORS. Pages asked for under a foreign name redirect to the device's address.

#include "esp_err.h"
#include "esp_http_server.h"

namespace web_ui {

esp_err_t init();

// The running HTTP server; null before init().
httpd_handle_t server();

// Registers an endpoint with the Host check above. route.user_ctx must be null.
esp_err_t register_route(const httpd_uri_t &route);

}  // namespace web_ui

#pragma once
// Spotify Web API client (section 4 of PLAN.md): token refresh with rotation,
// device lookup, play, repeat, volume, pause, transfer.
//
// The blocking calls do HTTPS and may store a rotated refresh token in NVS, so
// they must run on a task with an internal-RAM stack (never PSRAM) of at least
// 8 kB. The *_async variants queue the work for this module's own task.

#include <string>

#include "esp_err.h"
#include "settings.h"

namespace spotify_api {

// Starts the worker task.
esp_err_t init();

// --- blocking ---

// Exchanges the stored refresh token for an access token. Called before every
// alarm and whenever an API call answers 401.
esp_err_t refresh_access_token();

// Finds the Spotify Connect device called `name`; ESP_ERR_NOT_FOUND if absent.
esp_err_t find_device(const std::string &name, std::string &device_id);

// Section 4.1: refresh the token and look up this device (cached for later).
esp_err_t prepare(const settings::Config &cfg);

// Section 4.2: play cfg.track_uri on this device from the start (transferring
// playback first if needed), set repeat and volume 100 % (loudness is then up
// to the local gain). Uses the device found by prepare() if recent.
esp_err_t start_alarm_track(const settings::Config &cfg);

esp_err_t pause();

// --- asynchronous (results end up in last_result()) ---

void prepare_async();
void start_alarm_track_async();
void pause_async();

// Outcome of the latest prepare(); `seq` grows with every finished prepare.
struct PrepareResult {
    uint32_t seq;
    esp_err_t err;
};
PrepareResult prepare_result();

// Readable outcome of the last call, for the status page and log.
std::string last_result();

}  // namespace spotify_api

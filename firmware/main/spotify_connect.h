#pragma once
// cspot wrapper: makes this device a Spotify Connect speaker.
//
// First login comes from the Spotify app via zeroconf (mDNS service
// _spotify-connect._tcp, HTTP endpoint /spotify_info on the web UI's server).
// After any successful login, Spotify's reusable credentials are stored in NVS
// and used on every later boot, so no phone is needed again.
//
// PCM goes to audio_out (downmixed to mono). Outside alarm mode, Spotify's
// volume v (0..65535) sets the gain to (v / 65535)^2 * max_gain.
//
// The cspot session needs the Spotify client ID/secret from settings (for its
// CDN access token); they are read when a session starts.

#include <string>

#include "esp_err.h"

namespace spotify_connect {

// Loads stored credentials, registers the zeroconf endpoint on the web UI's
// server and starts the session task. Call after web_ui::init().
esp_err_t init();

// True while the cspot session is logged in to Spotify.
bool logged_in();

// Spotify user name of the current session; empty if not logged in.
std::string user_name();

// True while this device is the active Spotify Connect player.
bool active();

// True after Spotify paused playback (e.g. from the app), until it resumes or
// a new playback starts.
bool remote_paused();

// Alarm mode ignores Spotify's volume: the alarm owns the gain.
void set_alarm_mode(bool on);

// Erases the stored login and restarts, so the device waits for the app again.
void forget_credentials();

}  // namespace spotify_connect

#include "settings.h"

#include <cctype>
#include <cstring>
#include <mutex>
#include <vector>

#include "esp_check.h"
#include "esp_log.h"
#include "nvs.h"

namespace settings {

static const char *TAG = "settings";
static const char *NAMESPACE = "kallsup";

// NVS keys (at most 15 characters)
static const char *K_WIFI_SSID = "wifi_ssid";
static const char *K_WIFI_PASS = "wifi_pass";
static const char *K_TIME_ZONE = "tz";
static const char *K_ALARMS = "alarms";
static const char *K_TRACK_URI = "track_uri";
static const char *K_MAX_GAIN = "max_gain";
static const char *K_FADE_IN = "fade_in_s";
static const char *K_SNOOZE = "snooze_min";
static const char *K_AUTO_STOP = "auto_stop_min";
static const char *K_REPEAT = "repeat_track";
static const char *K_SP_CLIENT_ID = "sp_client_id";
static const char *K_SP_SECRET = "sp_secret";
static const char *K_SP_REFRESH = "sp_refresh";
static const char *K_DEVICE_NAME = "device_name";

static nvs_handle_t s_nvs;
static std::mutex s_mutex;  // guards s_cfg and NVS writes
static Config s_cfg;
static std::vector<Listener> s_listeners;

static void load_str(const char *key, std::string &out) {
    size_t len = 0;
    if (nvs_get_str(s_nvs, key, nullptr, &len) != ESP_OK || len == 0) return;
    std::string value(len, '\0');
    if (nvs_get_str(s_nvs, key, value.data(), &len) != ESP_OK) return;
    value.resize(len - 1);  // drop the terminating NUL
    out = value;
}

static void load_int(const char *key, int &out) {
    int32_t v;
    if (nvs_get_i32(s_nvs, key, &v) == ESP_OK) out = v;
}

static void load_bool(const char *key, bool &out) {
    uint8_t v;
    if (nvs_get_u8(s_nvs, key, &v) == ESP_OK) out = v != 0;
}

static void load_float(const char *key, float &out) {
    uint32_t bits;
    if (nvs_get_u32(s_nvs, key, &bits) == ESP_OK) memcpy(&out, &bits, sizeof(out));
}

// Alarms are one blob of 4 bytes each: enabled, hour, minute, weekdays.
static void load_alarms(std::array<Alarm, MAX_ALARMS> &alarms) {
    uint8_t buf[MAX_ALARMS * 4];
    size_t len = sizeof(buf);
    if (nvs_get_blob(s_nvs, K_ALARMS, buf, &len) != ESP_OK || len != sizeof(buf)) return;
    for (size_t i = 0; i < MAX_ALARMS; i++) {
        alarms[i] = {buf[4 * i] != 0, buf[4 * i + 1], buf[4 * i + 2], buf[4 * i + 3]};
    }
}

static esp_err_t store(const Config &c) {
    uint8_t alarms[MAX_ALARMS * 4];
    for (size_t i = 0; i < MAX_ALARMS; i++) {
        const Alarm &a = c.alarms[i];
        alarms[4 * i] = a.enabled;
        alarms[4 * i + 1] = a.hour;
        alarms[4 * i + 2] = a.minute;
        alarms[4 * i + 3] = a.weekdays;
    }
    uint32_t gain_bits;
    memcpy(&gain_bits, &c.max_gain, sizeof(gain_bits));

    ESP_RETURN_ON_ERROR(nvs_set_str(s_nvs, K_WIFI_SSID, c.wifi_ssid.c_str()), TAG, "wifi_ssid");
    ESP_RETURN_ON_ERROR(nvs_set_str(s_nvs, K_WIFI_PASS, c.wifi_pass.c_str()), TAG, "wifi_pass");
    ESP_RETURN_ON_ERROR(nvs_set_str(s_nvs, K_TIME_ZONE, c.time_zone.c_str()), TAG, "tz");
    ESP_RETURN_ON_ERROR(nvs_set_blob(s_nvs, K_ALARMS, alarms, sizeof(alarms)), TAG, "alarms");
    ESP_RETURN_ON_ERROR(nvs_set_str(s_nvs, K_TRACK_URI, c.track_uri.c_str()), TAG, "track_uri");
    ESP_RETURN_ON_ERROR(nvs_set_u32(s_nvs, K_MAX_GAIN, gain_bits), TAG, "max_gain");
    ESP_RETURN_ON_ERROR(nvs_set_i32(s_nvs, K_FADE_IN, c.fade_in_s), TAG, "fade_in_s");
    ESP_RETURN_ON_ERROR(nvs_set_i32(s_nvs, K_SNOOZE, c.snooze_min), TAG, "snooze_min");
    ESP_RETURN_ON_ERROR(nvs_set_i32(s_nvs, K_AUTO_STOP, c.auto_stop_min), TAG, "auto_stop_min");
    ESP_RETURN_ON_ERROR(nvs_set_u8(s_nvs, K_REPEAT, c.repeat_track), TAG, "repeat_track");
    ESP_RETURN_ON_ERROR(nvs_set_str(s_nvs, K_SP_CLIENT_ID, c.spotify_client_id.c_str()), TAG, "sp_client_id");
    ESP_RETURN_ON_ERROR(nvs_set_str(s_nvs, K_SP_SECRET, c.spotify_client_secret.c_str()), TAG, "sp_secret");
    ESP_RETURN_ON_ERROR(nvs_set_str(s_nvs, K_SP_REFRESH, c.spotify_refresh_token.c_str()), TAG, "sp_refresh");
    ESP_RETURN_ON_ERROR(nvs_set_str(s_nvs, K_DEVICE_NAME, c.device_name.c_str()), TAG, "device_name");
    return nvs_commit(s_nvs);
}

static bool is_track_uri(const std::string &uri) {
    static const char *PREFIX = "spotify:track:";
    const size_t plen = strlen(PREFIX);
    if (uri.size() != plen + 22 || uri.compare(0, plen, PREFIX) != 0) return false;
    for (size_t i = plen; i < uri.size(); i++) {
        if (!isalnum(static_cast<unsigned char>(uri[i]))) return false;
    }
    return true;
}

// Returns nullptr if valid, else the reason.
static const char *validate(const Config &c) {
    if (c.wifi_ssid.size() > 32) return "Wi-Fi SSID is longer than 32 characters";
    if (!c.wifi_pass.empty() && (c.wifi_pass.size() < 8 || c.wifi_pass.size() > 63)) {
        return "Wi-Fi password must be 8-63 characters (or empty for an open network)";
    }
    if (c.time_zone.empty() || c.time_zone.size() > 63) return "time zone must be 1-63 characters";
    for (const Alarm &a : c.alarms) {
        if (a.hour < 0 || a.hour > 23 || a.minute < 0 || a.minute > 59) return "alarm time out of range";
        if (a.weekdays < 0 || a.weekdays > 0x7F) return "alarm weekday mask out of range";
        if (a.enabled && a.weekdays == 0) return "an enabled alarm needs at least one weekday";
    }
    if (!c.track_uri.empty() && !is_track_uri(c.track_uri)) {
        return "track must look like spotify:track:<22 letters/digits>";
    }
    if (!(c.max_gain >= 0.0f && c.max_gain <= 1.0f)) return "max_gain must be between 0 and 1";
    if (c.fade_in_s < 0 || c.fade_in_s > 600) return "fade-in must be 0-600 s";
    if (c.snooze_min < 1 || c.snooze_min > 60) return "snooze must be 1-60 min";
    if (c.auto_stop_min < 1 || c.auto_stop_min > 240) return "auto-stop must be 1-240 min";
    if (c.spotify_client_id.size() > 64 || c.spotify_client_secret.size() > 64) {
        return "Spotify client ID/secret too long";
    }
    if (c.spotify_refresh_token.size() > 512) return "Spotify refresh token too long";
    if (c.device_name.empty() || c.device_name.size() > 32) return "device name must be 1-32 characters";
    return nullptr;
}

esp_err_t init() {
    ESP_RETURN_ON_ERROR(nvs_open(NAMESPACE, NVS_READWRITE, &s_nvs), TAG, "nvs_open");
    std::lock_guard<std::mutex> lock(s_mutex);
    Config &c = s_cfg;
    load_str(K_WIFI_SSID, c.wifi_ssid);
    load_str(K_WIFI_PASS, c.wifi_pass);
    load_str(K_TIME_ZONE, c.time_zone);
    load_alarms(c.alarms);
    load_str(K_TRACK_URI, c.track_uri);
    load_float(K_MAX_GAIN, c.max_gain);
    load_int(K_FADE_IN, c.fade_in_s);
    load_int(K_SNOOZE, c.snooze_min);
    load_int(K_AUTO_STOP, c.auto_stop_min);
    load_bool(K_REPEAT, c.repeat_track);
    load_str(K_SP_CLIENT_ID, c.spotify_client_id);
    load_str(K_SP_SECRET, c.spotify_client_secret);
    load_str(K_SP_REFRESH, c.spotify_refresh_token);
    load_str(K_DEVICE_NAME, c.device_name);
    if (const char *err = validate(c)) {
        ESP_LOGE(TAG, "stored settings invalid (%s), using defaults", err);
        c = Config{};
    }
    return ESP_OK;
}

Config get() {
    std::lock_guard<std::mutex> lock(s_mutex);
    return s_cfg;
}

esp_err_t save(const Config &cfg, std::string *error) {
    if (const char *err = validate(cfg)) {
        if (error) *error = err;
        return ESP_ERR_INVALID_ARG;
    }
    Config old_cfg;
    std::vector<Listener> listeners;
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        esp_err_t err = store(cfg);
        if (err != ESP_OK) {
            if (error) *error = "writing to flash failed";
            return err;
        }
        old_cfg = s_cfg;
        s_cfg = cfg;
        listeners = s_listeners;
    }
    ESP_LOGI(TAG, "saved");
    for (const Listener &l : listeners) l(old_cfg, cfg);
    return ESP_OK;
}

void on_change(Listener listener) {
    std::lock_guard<std::mutex> lock(s_mutex);
    s_listeners.push_back(std::move(listener));
}

}  // namespace settings

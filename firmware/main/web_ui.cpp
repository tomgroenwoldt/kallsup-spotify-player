#include "web_ui.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include "alarm.h"
#include "audio_out.h"
#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "lwip/sockets.h"
#include "net.h"
#include "settings.h"
#include "spotify_api.h"
#include "spotify_connect.h"

extern const char index_html_start[] asm("_binary_index_html_start");
extern const char index_html_end[] asm("_binary_index_html_end");
extern const char anleitung_html_start[] asm("_binary_anleitung_html_start");
extern const char anleitung_html_end[] asm("_binary_anleitung_html_end");

namespace web_ui {

static const char *TAG = "web_ui";

constexpr size_t MAX_BODY = 4096;
constexpr size_t OTA_CHUNK = 4096;
constexpr int64_t OTA_CONFIRM_DEADLINE_US = 5LL * 60 * 1000 * 1000;
static const char *PROJECT_NAME = "kallsup-alarm";

static httpd_handle_t s_server;

static esp_err_t send_json(httpd_req_t *req, cJSON *root) {
    char *text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!text) return httpd_resp_send_500(req);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_sendstr(req, text);
    cJSON_free(text);
    return err;
}

static esp_err_t send_error(httpd_req_t *req, const char *status, const std::string &message) {
    httpd_resp_set_status(req, status);
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "error", message.c_str());
    return send_json(req, root);
}

static esp_err_t send_ok(httpd_req_t *req) {
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    return send_json(req, root);
}

static bool is_json_request(httpd_req_t *req) {
    char type[64];
    if (httpd_req_get_hdr_value_str(req, "Content-Type", type, sizeof(type)) != ESP_OK) return false;
    return strncmp(type, "application/json", strlen("application/json")) == 0;
}

static esp_err_t read_body(httpd_req_t *req, std::string &body) {
    if (req->content_len > MAX_BODY) return ESP_ERR_INVALID_SIZE;
    body.resize(req->content_len);
    size_t got = 0;
    while (got < req->content_len) {
        const int r = httpd_req_recv(req, body.data() + got, req->content_len - got);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (r <= 0) return ESP_FAIL;
        got += r;
    }
    return ESP_OK;
}

// --- Host check, against DNS rebinding ---
//
// A website can point its own name at this device's IP. The browser then treats
// the page and this API as one origin, and CORS no longer protects it. Such
// requests carry the website's name in Host, so only IP literals and this
// device's own names are served.

static const char *LOCAL_DOMAINS[] = {"", ".local", ".fritz.box", ".lan", ".home", ".home.arpa", ".internal"};

static bool host_allowed(httpd_req_t *req) {
    const size_t len = httpd_req_get_hdr_value_len(req, "Host");
    if (len == 0) return true;  // no Host: not a browser
    if (len > 255) return false;
    std::string host(len + 1, '\0');
    if (httpd_req_get_hdr_value_str(req, "Host", host.data(), host.size()) != ESP_OK) return false;
    host.resize(len);
    for (char &c : host) c = tolower(static_cast<unsigned char>(c));
    if (host[0] == '[') return host.find(']') != std::string::npos;  // IPv6 literal
    host.resize(std::min(host.size(), host.find(':')));               // drop the port
    if (!host.empty() && host.back() == '.') host.pop_back();
    in_addr ip4;
    if (inet_pton(AF_INET, host.c_str(), &ip4) == 1) return true;
    for (const char *suffix : LOCAL_DOMAINS) {
        if (host == std::string(net::HOSTNAME) + suffix) return true;
    }
    return false;
}

// "http://<the address this request came in on>/", e.g. http://192.168.4.1/
// on the setup AP.
static std::string own_url(httpd_req_t *req) {
    sockaddr_in6 addr = {};
    socklen_t addr_len = sizeof(addr);
    char ip[INET6_ADDRSTRLEN] = "";
    if (getsockname(httpd_req_to_sockfd(req), reinterpret_cast<sockaddr *>(&addr), &addr_len) == 0) {
        if (addr.sin6_family == AF_INET) {
            inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in *>(&addr)->sin_addr, ip, sizeof(ip));
        } else if (addr.sin6_family == AF_INET6) {
            static const uint8_t V4_MAPPED[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};
            const uint8_t *b = addr.sin6_addr.s6_addr;
            if (memcmp(b, V4_MAPPED, sizeof(V4_MAPPED)) == 0) {
                inet_ntop(AF_INET, b + 12, ip, sizeof(ip));
            } else if (inet_ntop(AF_INET6, b, ip, sizeof(ip))) {
                return std::string("http://[") + ip + "]/";
            }
        }
    }
    if (!ip[0]) return std::string("http://") + net::HOSTNAME + ".local/";
    return std::string("http://") + ip + "/";
}

static esp_err_t redirect(httpd_req_t *req, const std::string &location) {
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", location.c_str());
    return httpd_resp_sendstr(req, "Weiter zur Einrichtung: <a href=\"/\">Kallsup Wecker</a>");
}

// Every route's handler, see register_route(). The real one is in user_ctx.
static esp_err_t guarded(httpd_req_t *req) {
    const auto handler = reinterpret_cast<esp_err_t (*)(httpd_req_t *)>(req->user_ctx);
    if (host_allowed(req)) return handler(req);
    ESP_LOGW(TAG, "refused %s: foreign Host header", req->uri);
    // A page on another name (a captive-portal check, say) moves to our address.
    if (req->method == HTTP_GET && strncmp(req->uri, "/api/", 5) != 0) return redirect(req, own_url(req));
    return send_error(req, "403 Forbidden", "unknown host name");
}

static std::string format_local(time_t t, const char *fmt) {
    struct tm tm;
    localtime_r(&t, &tm);
    char buf[48];
    strftime(buf, sizeof(buf), fmt, &tm);
    return buf;
}

// "heute 07:00", "morgen 07:00", or "Mo 27.10. 07:00".
static std::string format_alarm_de(time_t t, time_t now) {
    static const char *DAYS[] = {"So", "Mo", "Di", "Mi", "Do", "Fr", "Sa"};
    struct tm tm, today;
    localtime_r(&t, &tm);
    localtime_r(&now, &today);
    const time_t tomorrow_t = now + 24 * 3600;
    struct tm tomorrow;
    localtime_r(&tomorrow_t, &tomorrow);
    char buf[32];
    if (tm.tm_yday == today.tm_yday && tm.tm_year == today.tm_year) {
        snprintf(buf, sizeof(buf), "heute %02d:%02d", tm.tm_hour, tm.tm_min);
    } else if (tm.tm_yday == tomorrow.tm_yday && tm.tm_year == tomorrow.tm_year) {
        snprintf(buf, sizeof(buf), "morgen %02d:%02d", tm.tm_hour, tm.tm_min);
    } else {
        snprintf(buf, sizeof(buf), "%s %02d.%02d. %02d:%02d", DAYS[tm.tm_wday], tm.tm_mday, tm.tm_mon + 1,
                 tm.tm_hour, tm.tm_min);
    }
    return buf;
}

// --- GET / and /anleitung ---

static esp_err_t get_index(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    // EMBED_TXTFILES appends a NUL terminator
    return httpd_resp_send(req, index_html_start, index_html_end - index_html_start - 1);
}

static esp_err_t get_guide(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, anleitung_html_start, anleitung_html_end - anleitung_html_start - 1);
}

// Anything else, e.g. a phone's captive-portal check on the setup AP, goes to
// the main page, by address if it came in under a foreign name. iOS only shows
// the portal if the response has a body.
static esp_err_t redirect_404(httpd_req_t *req, httpd_err_code_t) {
    return redirect(req, host_allowed(req) ? "/" : own_url(req));
}

// --- GET /api/wifi/scan ---

// Only for the setup page: a scan blocks this server for ~2 s.
static esp_err_t get_wifi_scan(httpd_req_t *req) {
    if (!net::status().ap_active) return send_error(req, "409 Conflict", "only while the setup Wi-Fi is on");
    wifi_scan_config_t scan = {};
    esp_err_t err = esp_wifi_scan_start(&scan, true);  // blocks ~2 s
    if (err != ESP_OK) {
        return send_error(req, "503 Service Unavailable", std::string("scan failed: ") + esp_err_to_name(err));
    }
    uint16_t count = 0;
    esp_wifi_scan_get_ap_num(&count);
    std::vector<wifi_ap_record_t> aps(count);
    if (count) esp_wifi_scan_get_ap_records(&count, aps.data());
    aps.resize(count);
    std::sort(aps.begin(), aps.end(), [](const auto &a, const auto &b) { return a.rssi > b.rssi; });

    cJSON *root = cJSON_CreateObject();
    cJSON *list = cJSON_AddArrayToObject(root, "networks");
    std::vector<std::string> seen;
    for (const wifi_ap_record_t &ap : aps) {
        const std::string ssid(reinterpret_cast<const char *>(ap.ssid));
        if (ssid.empty() || std::find(seen.begin(), seen.end(), ssid) != seen.end()) continue;
        seen.push_back(ssid);
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "ssid", ssid.c_str());
        cJSON_AddNumberToObject(item, "rssi", ap.rssi);
        cJSON_AddBoolToObject(item, "locked", ap.authmode != WIFI_AUTH_OPEN);
        cJSON_AddItemToArray(list, item);
    }
    return send_json(req, root);
}

// --- GET /api/settings ---

static esp_err_t get_settings(httpd_req_t *req) {
    const settings::Config c = settings::get();
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "wifi_ssid", c.wifi_ssid.c_str());
    cJSON_AddBoolToObject(root, "wifi_pass_set", !c.wifi_pass.empty());
    cJSON_AddStringToObject(root, "time_zone", c.time_zone.c_str());
    cJSON *alarms = cJSON_AddArrayToObject(root, "alarms");
    for (const settings::Alarm &a : c.alarms) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddBoolToObject(o, "enabled", a.enabled);
        cJSON_AddNumberToObject(o, "hour", a.hour);
        cJSON_AddNumberToObject(o, "minute", a.minute);
        cJSON_AddNumberToObject(o, "weekdays", a.weekdays);
        cJSON_AddItemToArray(alarms, o);
    }
    cJSON_AddStringToObject(root, "track_uri", c.track_uri.c_str());
    cJSON_AddNumberToObject(root, "max_gain", std::round(c.max_gain * 100) / 100.0);  // 0.6f, not 0.6000000238
    cJSON_AddNumberToObject(root, "fade_in_s", c.fade_in_s);
    cJSON_AddNumberToObject(root, "snooze_min", c.snooze_min);
    cJSON_AddNumberToObject(root, "auto_stop_min", c.auto_stop_min);
    cJSON_AddBoolToObject(root, "repeat_track", c.repeat_track);
    cJSON_AddStringToObject(root, "spotify_client_id", c.spotify_client_id.c_str());
    cJSON_AddBoolToObject(root, "spotify_client_secret_set", !c.spotify_client_secret.empty());
    cJSON_AddBoolToObject(root, "spotify_refresh_token_set", !c.spotify_refresh_token.empty());
    cJSON_AddStringToObject(root, "device_name", c.device_name.c_str());
    cJSON_AddBoolToObject(root, "display_enabled", c.display_enabled);
    cJSON_AddBoolToObject(root, "face_enabled", c.face_enabled);
    cJSON_AddNumberToObject(root, "face_after_s", c.face_after_s);
    cJSON_AddNumberToObject(root, "night_start_h", c.night_start_h);
    cJSON_AddNumberToObject(root, "night_end_h", c.night_end_h);
    cJSON_AddNumberToObject(root, "display_brightness", c.display_brightness);
    return send_json(req, root);
}

// --- POST /api/settings ---

static void read_string(const cJSON *o, const char *key, std::string &out) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    if (cJSON_IsString(v)) out = v->valuestring;
}

// Secrets are never sent to the browser, so an empty field means "keep".
static void read_secret(const cJSON *o, const char *key, std::string &out) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    if (cJSON_IsNull(v)) {
        out.clear();
    } else if (cJSON_IsString(v) && v->valuestring[0] != '\0') {
        out = v->valuestring;
    }
}

static void read_int(const cJSON *o, const char *key, int &out) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    if (cJSON_IsNumber(v)) out = v->valueint;
}

static void read_bool(const cJSON *o, const char *key, bool &out) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    if (cJSON_IsBool(v)) out = cJSON_IsTrue(v);
}

static esp_err_t post_settings(httpd_req_t *req) {
    if (!is_json_request(req)) return send_error(req, "415 Unsupported Media Type", "expected application/json");
    std::string body;
    if (read_body(req, body) != ESP_OK) return send_error(req, "400 Bad Request", "body missing or too large");
    cJSON *root = cJSON_ParseWithLength(body.data(), body.size());
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return send_error(req, "400 Bad Request", "invalid JSON");
    }

    settings::Config c = settings::get();
    read_string(root, "wifi_ssid", c.wifi_ssid);
    read_secret(root, "wifi_pass", c.wifi_pass);
    read_string(root, "time_zone", c.time_zone);
    const cJSON *alarms = cJSON_GetObjectItemCaseSensitive(root, "alarms");
    if (cJSON_IsArray(alarms)) {
        size_t i = 0;
        const cJSON *o;
        cJSON_ArrayForEach(o, alarms) {
            if (i >= settings::MAX_ALARMS) break;
            settings::Alarm &a = c.alarms[i++];
            read_bool(o, "enabled", a.enabled);
            read_int(o, "hour", a.hour);
            read_int(o, "minute", a.minute);
            read_int(o, "weekdays", a.weekdays);
        }
    }
    read_string(root, "track_uri", c.track_uri);
    const cJSON *gain = cJSON_GetObjectItemCaseSensitive(root, "max_gain");
    if (cJSON_IsNumber(gain)) c.max_gain = gain->valuedouble;
    read_int(root, "fade_in_s", c.fade_in_s);
    read_int(root, "snooze_min", c.snooze_min);
    read_int(root, "auto_stop_min", c.auto_stop_min);
    read_bool(root, "repeat_track", c.repeat_track);
    read_string(root, "spotify_client_id", c.spotify_client_id);
    read_secret(root, "spotify_client_secret", c.spotify_client_secret);
    read_secret(root, "spotify_refresh_token", c.spotify_refresh_token);
    read_string(root, "device_name", c.device_name);
    read_bool(root, "display_enabled", c.display_enabled);
    read_bool(root, "face_enabled", c.face_enabled);
    read_int(root, "face_after_s", c.face_after_s);
    read_int(root, "night_start_h", c.night_start_h);
    read_int(root, "night_end_h", c.night_end_h);
    read_int(root, "display_brightness", c.display_brightness);
    cJSON_Delete(root);

    std::string error;
    if (settings::save(c, &error) != ESP_OK) return send_error(req, "400 Bad Request", error);
    return send_ok(req);
}

// --- GET /api/status ---

static const char *reset_reason_name(esp_reset_reason_t r) {
    switch (r) {
        case ESP_RST_POWERON: return "power on";
        case ESP_RST_EXT: return "reset pin";
        case ESP_RST_SW: return "restart";
        case ESP_RST_PANIC: return "crash";
        case ESP_RST_INT_WDT: return "interrupt watchdog";
        case ESP_RST_TASK_WDT: return "task watchdog";
        case ESP_RST_WDT: return "watchdog";
        case ESP_RST_BROWNOUT: return "brownout";
        case ESP_RST_USB: return "usb";
        default: return "other";
    }
}

static esp_err_t get_status(httpd_req_t *req) {
    const settings::Config c = settings::get();
    const time_t now = time(nullptr);
    const bool synced = net::time_synced();
    cJSON *root = cJSON_CreateObject();

    cJSON_AddStringToObject(root, "time", format_local(now, "%Y-%m-%d %H:%M:%S %Z").c_str());
    cJSON_AddBoolToObject(root, "time_synced", synced);

    const net::Status ns = net::status();
    cJSON *wifi = cJSON_AddObjectToObject(root, "wifi");
    cJSON_AddBoolToObject(wifi, "connected", ns.sta_connected);
    cJSON_AddStringToObject(wifi, "ssid", ns.ssid.c_str());
    cJSON_AddStringToObject(wifi, "ip", ns.ip.c_str());
    cJSON_AddNumberToObject(wifi, "rssi", ns.rssi);
    cJSON_AddBoolToObject(wifi, "ap_active", ns.ap_active);
    cJSON_AddStringToObject(wifi, "error", ns.error == net::WifiError::WRONG_PASSWORD ? "wrong_password"
                                           : ns.error == net::WifiError::NOT_FOUND    ? "not_found"
                                           : ns.error == net::WifiError::OTHER        ? "other"
                                                                                      : "");

    cJSON *spotify = cJSON_AddObjectToObject(root, "spotify");
    cJSON_AddBoolToObject(spotify, "logged_in", spotify_connect::logged_in());
    cJSON_AddStringToObject(spotify, "user", spotify_connect::user_name().c_str());
    cJSON_AddBoolToObject(spotify, "active", spotify_connect::active());
    cJSON_AddStringToObject(spotify, "api_result", spotify_api::last_result().c_str());
    cJSON_AddBoolToObject(spotify, "api_credentials_set",
                          !c.spotify_client_id.empty() && !c.spotify_client_secret.empty() &&
                              !c.spotify_refresh_token.empty());

    // Without a synced clock the next alarm can't be known.
    const time_t next = synced ? alarm_clock::next_alarm(c, now) : 0;
    if (next) {
        cJSON_AddStringToObject(root, "next_alarm", format_alarm_de(next, now).c_str());
    } else {
        cJSON_AddNullToObject(root, "next_alarm");
    }

    cJSON_AddStringToObject(root, "alarm_state", alarm_clock::state_name(alarm_clock::state()));
    if (const time_t until = alarm_clock::snoozed_until()) {
        cJSON_AddStringToObject(root, "snoozed_until", format_local(until, "%H:%M").c_str());
    }
    cJSON_AddBoolToObject(root, "amp_on", audio_out::amp_enabled());
    cJSON_AddNumberToObject(root, "uptime_s", (double)(esp_timer_get_time() / 1000000));
    cJSON_AddStringToObject(root, "reset_reason", reset_reason_name(esp_reset_reason()));
    cJSON_AddNumberToObject(root, "heap_internal", heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(root, "heap_internal_min", heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(root, "heap_psram", heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    cJSON_AddStringToObject(root, "version", esp_app_get_description()->version);
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t ota_state;
    const bool has_state = esp_ota_get_state_partition(running, &ota_state) == ESP_OK;
    cJSON_AddStringToObject(root, "partition", running->label);
    cJSON_AddStringToObject(root, "ota_state", !has_state ? "factory"
                                               : ota_state == ESP_OTA_IMG_PENDING_VERIFY ? "pending verify"
                                               : ota_state == ESP_OTA_IMG_VALID ? "valid"
                                                                                : "other");
    return send_json(req, root);
}

// --- POST /api/test/{alarm,fallback,stop} ---

static esp_err_t post_test(httpd_req_t *req) {
    if (!is_json_request(req)) return send_error(req, "415 Unsupported Media Type", "expected application/json");
    const char *action = req->uri + strlen("/api/test/");
    if (strcmp(action, "stop") == 0) {
        alarm_clock::stop();
    } else if (strcmp(action, "snooze") == 0) {
        alarm_clock::snooze();
    } else if (strcmp(action, "fallback") == 0) {
        alarm_clock::ring_fallback();
    } else if (strcmp(action, "alarm") == 0) {
        alarm_clock::ring_now();
    } else {
        return send_error(req, "404 Not Found", "unknown test");
    }
    return send_ok(req);
}

// --- POST /api/ota ---

static void restart_cb(void *) {
    esp_restart();
}

static void restart_soon() {
    static esp_timer_handle_t timer;
    if (!timer) {
        esp_timer_create_args_t args = {};
        args.callback = restart_cb;
        args.name = "restart";
        esp_timer_create(&args, &timer);
    }
    esp_timer_start_once(timer, 1000 * 1000);  // let the response go out first
}

#if !CONFIG_SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT && !CONFIG_SECURE_BOOT
#error "OTA must only accept signed images: set CONFIG_SECURE_SIGNED_APPS_NO_SECURE_BOOT (see sdkconfig.defaults)"
#endif

// Firmware upload: the raw .bin as application/octet-stream. That content type
// needs a CORS preflight, so other websites can't post one through a browser.
// esp_ota_end() only accepts images signed with our key.
static esp_err_t post_ota(httpd_req_t *req) {
    char type[64];
    if (httpd_req_get_hdr_value_str(req, "Content-Type", type, sizeof(type)) != ESP_OK ||
        strcmp(type, "application/octet-stream") != 0) {
        return send_error(req, "415 Unsupported Media Type", "expected application/octet-stream");
    }
    if (alarm_clock::ringing()) return send_error(req, "409 Conflict", "an alarm is ringing");
    const esp_partition_t *part = esp_ota_get_next_update_partition(nullptr);
    if (!part) return send_error(req, "500 Internal Server Error", "no OTA partition");
    if (req->content_len == 0 || req->content_len > part->size) {
        return send_error(req, "400 Bad Request", "image missing or larger than the partition");
    }

    ESP_LOGI(TAG, "OTA: receiving %u bytes into %s", (unsigned)req->content_len, part->label);
    esp_ota_handle_t ota;
    if (esp_ota_begin(part, OTA_WITH_SEQUENTIAL_WRITES, &ota) != ESP_OK) {
        return send_error(req, "500 Internal Server Error", "esp_ota_begin failed");
    }
    std::string buf(OTA_CHUNK, '\0');
    size_t got = 0;
    esp_err_t err = ESP_OK;
    while (got < req->content_len) {
        const int r = httpd_req_recv(req, buf.data(), std::min(OTA_CHUNK, req->content_len - got));
        if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (r <= 0) {
            err = ESP_FAIL;
            break;
        }
        err = esp_ota_write(ota, buf.data(), r);
        if (err != ESP_OK) break;
        got += r;
    }
    if (err != ESP_OK) {
        esp_ota_abort(ota);
        return send_error(req, "400 Bad Request", std::string("upload failed: ") + esp_err_to_name(err));
    }
    err = esp_ota_end(ota);  // verifies the checksum and signature
    if (err != ESP_OK) {
        return send_error(req, "400 Bad Request", std::string("invalid or unsigned image: ") + esp_err_to_name(err));
    }
    esp_app_desc_t desc;
    if (esp_ota_get_partition_description(part, &desc) != ESP_OK || strcmp(desc.project_name, PROJECT_NAME) != 0) {
        return send_error(req, "400 Bad Request", "not a kallsup-alarm firmware image");
    }
    err = esp_ota_set_boot_partition(part);
    if (err != ESP_OK) {
        return send_error(req, "500 Internal Server Error", std::string("set boot partition: ") + esp_err_to_name(err));
    }
    ESP_LOGW(TAG, "OTA: version %s written to %s, restarting", desc.version, part->label);
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddStringToObject(root, "version", desc.version);
    send_json(req, root);
    restart_soon();
    return ESP_OK;
}

// After an OTA update the new image runs "pending verify": it confirms itself
// once Wi-Fi and time work, else it restarts and the bootloader rolls back.
static void ota_confirm_cb(void *arg) {
    if (net::status().sta_connected && net::time_synced()) {
        esp_ota_mark_app_valid_cancel_rollback();
        ESP_LOGI(TAG, "OTA: new firmware confirmed");
        esp_timer_delete(*static_cast<esp_timer_handle_t *>(arg));
    } else if (esp_timer_get_time() > OTA_CONFIRM_DEADLINE_US) {
        ESP_LOGE(TAG, "OTA: new firmware not healthy after 5 min, rolling back");
        esp_restart();
    }
}

static void start_ota_confirmation() {
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(esp_ota_get_running_partition(), &state) != ESP_OK ||
        state != ESP_OTA_IMG_PENDING_VERIFY) {
        return;
    }
    ESP_LOGW(TAG, "OTA: running new firmware, waiting for Wi-Fi and time before confirming it");
    static esp_timer_handle_t timer;
    esp_timer_create_args_t args = {};
    args.callback = ota_confirm_cb;
    args.arg = &timer;
    args.name = "ota_confirm";
    if (esp_timer_create(&args, &timer) == ESP_OK) esp_timer_start_periodic(timer, 5 * 1000 * 1000);
}

esp_err_t init() {
    start_ota_confirmation();
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 8192;
    config.lru_purge_enable = true;
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.max_uri_handlers = 16;
    ESP_RETURN_ON_ERROR(httpd_start(&s_server, &config), TAG, "httpd_start");

    const httpd_uri_t routes[] = {
        {.uri = "/", .method = HTTP_GET, .handler = get_index, .user_ctx = nullptr},
        {.uri = "/anleitung", .method = HTTP_GET, .handler = get_guide, .user_ctx = nullptr},
        {.uri = "/api/wifi/scan", .method = HTTP_GET, .handler = get_wifi_scan, .user_ctx = nullptr},
        {.uri = "/api/settings", .method = HTTP_GET, .handler = get_settings, .user_ctx = nullptr},
        {.uri = "/api/settings", .method = HTTP_POST, .handler = post_settings, .user_ctx = nullptr},
        {.uri = "/api/status", .method = HTTP_GET, .handler = get_status, .user_ctx = nullptr},
        {.uri = "/api/test/*", .method = HTTP_POST, .handler = post_test, .user_ctx = nullptr},
        {.uri = "/api/ota", .method = HTTP_POST, .handler = post_ota, .user_ctx = nullptr},
    };
    for (const httpd_uri_t &route : routes) {
        ESP_RETURN_ON_ERROR(register_route(route), TAG, "route %s", route.uri);
    }
    ESP_RETURN_ON_ERROR(httpd_register_err_handler(s_server, HTTPD_404_NOT_FOUND, redirect_404), TAG, "404");
    ESP_LOGI(TAG, "listening on port %d", config.server_port);
    return ESP_OK;
}

httpd_handle_t server() {
    return s_server;
}

esp_err_t register_route(const httpd_uri_t &route) {
    ESP_RETURN_ON_FALSE(s_server && !route.user_ctx, ESP_ERR_INVALID_ARG, TAG, "register_route");
    httpd_uri_t wrapped = route;
    wrapped.handler = guarded;
    wrapped.user_ctx = reinterpret_cast<void *>(route.handler);
    return httpd_register_uri_handler(s_server, &wrapped);
}

}  // namespace web_ui

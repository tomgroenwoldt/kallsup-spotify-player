#include "spotify_api.h"

#include <cctype>
#include <cstdio>
#include <cstring>
#include <mutex>

#include "cJSON.h"
#include "esp_check.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"

namespace spotify_api {

static const char *TAG = "spotify_api";

static const char *API_BASE = "https://api.spotify.com/v1";
static const char *TOKEN_URL = "https://accounts.spotify.com/api/token";
constexpr int TIMEOUT_MS = 10000;
constexpr int CONNECT_ATTEMPTS = 3;
constexpr size_t MAX_BODY = 16 * 1024;
constexpr int64_t TOKEN_MARGIN_US = 60LL * 1000 * 1000;  // refresh a minute before expiry
constexpr uint32_t TASK_STACK = 10 * 1024;               // internal RAM: may write NVS
constexpr int64_t DEVICE_CACHE_US = 10LL * 60 * 1000 * 1000;

enum class Job { PREPARE, START_ALARM_TRACK, PAUSE };

static QueueHandle_t s_jobs;
static std::mutex s_mutex;  // guards the fields below
static std::string s_access_token;
static int64_t s_token_expires_us = 0;
static std::string s_last_result;
static std::string s_device_id;  // from the last successful lookup
static int64_t s_device_found_us = 0;
static PrepareResult s_prepare_result = {0, ESP_OK};

static void set_result(esp_err_t err, const std::string &text) {
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "%s", text.c_str());
    } else {
        ESP_LOGE(TAG, "%s", text.c_str());
    }
    std::lock_guard<std::mutex> lock(s_mutex);
    s_last_result = text;
}

// --- HTTP ---

struct Response {
    int status = 0;
    std::string body;
};

static esp_err_t http_once(esp_http_client_method_t method, const std::string &url, const std::string &auth,
                           const char *content_type, const std::string &body, Response &resp) {
    esp_http_client_config_t cfg = {};
    cfg.url = url.c_str();
    cfg.method = method;
    cfg.timeout_ms = TIMEOUT_MS;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.buffer_size_tx = 1024;  // the Authorization header alone is ~300 bytes
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return ESP_ERR_NO_MEM;
    esp_http_client_set_header(client, "Authorization", auth.c_str());
    if (content_type) esp_http_client_set_header(client, "Content-Type", content_type);

    // Content-Length is always sent, also 0 for PUTs without a body.
    const char *method_name = method == HTTP_METHOD_GET ? "GET" : method == HTTP_METHOD_POST ? "POST" : "PUT";
    const std::string url_no_query = url.substr(0, url.find('?'));
    const int64_t t0 = esp_timer_get_time();
    esp_err_t err = esp_http_client_open(client, body.size());
    if (err == ESP_OK && !body.empty() && esp_http_client_write(client, body.data(), body.size()) != (int)body.size()) {
        err = ESP_FAIL;
    }
    if (err == ESP_OK && esp_http_client_fetch_headers(client) < 0) err = ESP_FAIL;
    if (err == ESP_OK) {
        resp.status = esp_http_client_get_status_code(client);
        char buf[512];
        int n;
        while ((n = esp_http_client_read(client, buf, sizeof(buf))) > 0) {
            if (resp.body.size() + n > MAX_BODY) {
                err = ESP_ERR_INVALID_SIZE;
                break;
            }
            resp.body.append(buf, n);
        }
        if (n < 0) err = ESP_FAIL;
    }
    ESP_LOGI(TAG, "%s %s -> %d (%s, %lld ms)", method_name, url_no_query.c_str(), resp.status, esp_err_to_name(err),
             (esp_timer_get_time() - t0) / 1000);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return err;
}

// A failed connect (DNS, TCP or TLS handshake) sent nothing yet, so it is safe to retry;
// a weak Wi-Fi link drops one now and then.
static esp_err_t http(esp_http_client_method_t method, const std::string &url, const std::string &auth,
                      const char *content_type, const std::string &body, Response &resp) {
    esp_err_t err = ESP_OK;
    for (int attempt = 1; attempt <= CONNECT_ATTEMPTS; attempt++) {
        resp = Response{};
        err = http_once(method, url, auth, content_type, body, resp);
        if (err != ESP_ERR_HTTP_CONNECT || attempt == CONNECT_ATTEMPTS) break;
        ESP_LOGW(TAG, "connect failed (attempt %d/%d), retrying", attempt, CONNECT_ATTEMPTS);
        vTaskDelay(pdMS_TO_TICKS(1000 * attempt));
    }
    return err;
}

// "<status> <reason/message>" from a Spotify error response.
static std::string describe(const Response &resp) {
    std::string text = std::to_string(resp.status);
    cJSON *root = cJSON_ParseWithLength(resp.body.data(), resp.body.size());
    const cJSON *error = cJSON_GetObjectItem(root, "error");
    const cJSON *desc = cJSON_GetObjectItem(root, "error_description");  // accounts service
    const cJSON *message = cJSON_IsObject(error) ? cJSON_GetObjectItem(error, "message") : nullptr;
    const cJSON *reason = cJSON_IsObject(error) ? cJSON_GetObjectItem(error, "reason") : nullptr;
    if (cJSON_IsString(reason)) {
        text += std::string(" ") + reason->valuestring;
        if (strcmp(reason->valuestring, "PREMIUM_REQUIRED") == 0) text += " (has Spotify Premium lapsed?)";
    } else if (cJSON_IsString(message)) {
        text += std::string(" ") + message->valuestring;
    } else if (cJSON_IsString(error)) {
        text += std::string(" ") + error->valuestring;
        if (cJSON_IsString(desc)) text += std::string(": ") + desc->valuestring;
    }
    cJSON_Delete(root);
    return text;
}

static std::string url_encode(const std::string &in) {
    std::string out;
    for (unsigned char c : in) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out += (char)c;
        } else {
            char hex[4];
            snprintf(hex, sizeof(hex), "%%%02X", c);
            out += hex;
        }
    }
    return out;
}

// --- token ---

esp_err_t refresh_access_token() {
    settings::Config cfg = settings::get();
    if (cfg.spotify_client_id.empty() || cfg.spotify_client_secret.empty() || cfg.spotify_refresh_token.empty()) {
        set_result(ESP_ERR_INVALID_STATE, "Web API not set up: client ID, secret and refresh token needed");
        return ESP_ERR_INVALID_STATE;
    }

    const std::string pair = cfg.spotify_client_id + ":" + cfg.spotify_client_secret;
    unsigned char b64[256];
    size_t b64_len = 0;
    if (mbedtls_base64_encode(b64, sizeof(b64), &b64_len, (const unsigned char *)pair.data(), pair.size()) != 0) {
        set_result(ESP_ERR_INVALID_SIZE, "client ID/secret too long");
        return ESP_ERR_INVALID_SIZE;
    }
    const std::string auth = "Basic " + std::string((const char *)b64, b64_len);
    const std::string body = "grant_type=refresh_token&refresh_token=" + url_encode(cfg.spotify_refresh_token);

    Response resp;
    esp_err_t err = http(HTTP_METHOD_POST, TOKEN_URL, auth, "application/x-www-form-urlencoded", body, resp);
    if (err != ESP_OK) {
        set_result(err, std::string("token refresh: network error ") + esp_err_to_name(err));
        return err;
    }
    if (resp.status != 200) {
        set_result(ESP_FAIL, "token refresh failed: " + describe(resp));
        return ESP_FAIL;
    }

    cJSON *root = cJSON_ParseWithLength(resp.body.data(), resp.body.size());
    const cJSON *access = cJSON_GetObjectItem(root, "access_token");
    const cJSON *expires = cJSON_GetObjectItem(root, "expires_in");
    const cJSON *refresh = cJSON_GetObjectItem(root, "refresh_token");
    if (!cJSON_IsString(access) || !cJSON_IsNumber(expires)) {
        cJSON_Delete(root);
        set_result(ESP_FAIL, "token refresh: unexpected response");
        return ESP_FAIL;
    }
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        s_access_token = access->valuestring;
        s_token_expires_us = esp_timer_get_time() + (int64_t)expires->valueint * 1000000 - TOKEN_MARGIN_US;
    }
    // Spotify may rotate the refresh token; the old one then stops working.
    if (cJSON_IsString(refresh) && cfg.spotify_refresh_token != refresh->valuestring) {
        cfg.spotify_refresh_token = refresh->valuestring;
        std::string error;
        if (settings::save(cfg, &error) == ESP_OK) {
            ESP_LOGI(TAG, "refresh token rotated, stored the new one");
        } else {
            ESP_LOGE(TAG, "storing the rotated refresh token failed: %s", error.c_str());
        }
    }
    cJSON_Delete(root);
    ESP_LOGI(TAG, "access token refreshed");
    return ESP_OK;
}

// An authorized API call; a 401 refreshes the token and retries once.
static esp_err_t api(esp_http_client_method_t method, const std::string &path, const std::string &json,
                     Response &resp) {
    for (int attempt = 0; attempt < 2; attempt++) {
        std::string token;
        {
            std::lock_guard<std::mutex> lock(s_mutex);
            if (attempt == 0 && esp_timer_get_time() < s_token_expires_us) token = s_access_token;
        }
        if (token.empty()) {
            ESP_RETURN_ON_ERROR(refresh_access_token(), TAG, "token");
            std::lock_guard<std::mutex> lock(s_mutex);
            token = s_access_token;
        }
        resp = Response{};
        const esp_err_t err = http(method, std::string(API_BASE) + path, "Bearer " + token,
                                   json.empty() ? nullptr : "application/json", json, resp);
        if (err != ESP_OK) {
            set_result(err, path + ": network error " + esp_err_to_name(err));
            return err;
        }
        if (resp.status != 401) return ESP_OK;
        ESP_LOGW(TAG, "access token rejected, refreshing");
    }
    return ESP_OK;
}

static bool ok_status(int status) {
    return status >= 200 && status < 300;
}

// --- player calls ---

esp_err_t find_device(const std::string &name, std::string &device_id) {
    Response resp;
    ESP_RETURN_ON_ERROR(api(HTTP_METHOD_GET, "/me/player/devices", "", resp), TAG, "devices");
    if (resp.status != 200) {
        set_result(ESP_FAIL, "listing devices failed: " + describe(resp));
        return ESP_FAIL;
    }
    cJSON *root = cJSON_ParseWithLength(resp.body.data(), resp.body.size());
    std::string seen;
    const cJSON *dev;
    cJSON_ArrayForEach(dev, cJSON_GetObjectItem(root, "devices")) {
        const cJSON *dev_name = cJSON_GetObjectItem(dev, "name");
        const cJSON *id = cJSON_GetObjectItem(dev, "id");
        if (!cJSON_IsString(dev_name) || !cJSON_IsString(id)) continue;
        if (name == dev_name->valuestring) device_id = id->valuestring;
        seen += seen.empty() ? "" : ", ";
        seen += dev_name->valuestring;
    }
    cJSON_Delete(root);
    if (device_id.empty()) {
        set_result(ESP_ERR_NOT_FOUND, "device \"" + name + "\" not in Spotify's device list (seen: " +
                                          (seen.empty() ? "none" : seen) + "); is it logged in?");
        return ESP_ERR_NOT_FOUND;
    }
    std::lock_guard<std::mutex> lock(s_mutex);
    s_device_id = device_id;
    s_device_found_us = esp_timer_get_time();
    return ESP_OK;
}

static std::string cached_device_id() {
    std::lock_guard<std::mutex> lock(s_mutex);
    if (s_device_id.empty() || esp_timer_get_time() - s_device_found_us > DEVICE_CACHE_US) return "";
    return s_device_id;
}

esp_err_t prepare(const settings::Config &cfg) {
    esp_err_t err = refresh_access_token();
    std::string device_id;
    if (err == ESP_OK) err = find_device(cfg.device_name, device_id);
    if (err == ESP_OK) set_result(ESP_OK, "ready: \"" + cfg.device_name + "\" found");
    std::lock_guard<std::mutex> lock(s_mutex);
    s_prepare_result = {s_prepare_result.seq + 1, err};
    return err;
}

static esp_err_t play_uri(const std::string &device_id, const std::string &uri, Response &resp) {
    cJSON *body = cJSON_CreateObject();
    cJSON *uris = cJSON_AddArrayToObject(body, "uris");
    cJSON_AddItemToArray(uris, cJSON_CreateString(uri.c_str()));
    cJSON_AddNumberToObject(body, "position_ms", 0);
    char *json = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    const std::string payload = json ? json : "";
    cJSON_free(json);
    return api(HTTP_METHOD_PUT, "/me/player/play?device_id=" + device_id, payload, resp);
}

static esp_err_t transfer(const std::string &device_id) {
    Response resp;
    const std::string body = R"({"device_ids":[")" + device_id + R"("],"play":false})";
    ESP_RETURN_ON_ERROR(api(HTTP_METHOD_PUT, "/me/player", body, resp), TAG, "transfer");
    if (!ok_status(resp.status)) {
        set_result(ESP_FAIL, "transfer failed: " + describe(resp));
        return ESP_FAIL;
    }
    return ESP_OK;
}

// A player setting that isn't worth failing the alarm over.
static void best_effort(const std::string &path) {
    Response resp;
    if (api(HTTP_METHOD_PUT, path, "", resp) == ESP_OK && !ok_status(resp.status)) {
        ESP_LOGW(TAG, "%s: %s", path.c_str(), describe(resp).c_str());
    }
}

esp_err_t start_alarm_track(const settings::Config &cfg) {
    if (cfg.track_uri.empty()) {
        set_result(ESP_ERR_INVALID_STATE, "no track set");
        return ESP_ERR_INVALID_STATE;
    }
    const int64_t started = esp_timer_get_time();
    std::string device_id = cached_device_id();
    if (device_id.empty()) ESP_RETURN_ON_ERROR(find_device(cfg.device_name, device_id), TAG, "device");

    Response resp;
    ESP_RETURN_ON_ERROR(play_uri(device_id, cfg.track_uri, resp), TAG, "play");
    if (!ok_status(resp.status)) {
        ESP_LOGW(TAG, "play: %s, transferring playback first", describe(resp).c_str());
        // The cached id may be stale (the device re-registered): look it up again.
        ESP_RETURN_ON_ERROR(find_device(cfg.device_name, device_id), TAG, "device");
        ESP_RETURN_ON_ERROR(transfer(device_id), TAG, "transfer");
        ESP_RETURN_ON_ERROR(play_uri(device_id, cfg.track_uri, resp), TAG, "play");
        if (!ok_status(resp.status)) {
            set_result(ESP_FAIL, "play failed: " + describe(resp));
            return ESP_FAIL;
        }
    }
    best_effort("/me/player/repeat?state=" + std::string(cfg.repeat_track ? "track" : "off") +
                "&device_id=" + device_id);
    best_effort("/me/player/volume?volume_percent=100&device_id=" + device_id);

    set_result(ESP_OK, "playing " + cfg.track_uri + " on \"" + cfg.device_name + "\" (" +
                           std::to_string((esp_timer_get_time() - started) / 1000) + " ms)");
    return ESP_OK;
}

esp_err_t pause() {
    Response resp;
    ESP_RETURN_ON_ERROR(api(HTTP_METHOD_PUT, "/me/player/pause", "", resp), TAG, "pause");
    // 403/404 here usually means nothing is playing, which is fine for a pause.
    if (!ok_status(resp.status) && resp.status != 403 && resp.status != 404) {
        set_result(ESP_FAIL, "pause failed: " + describe(resp));
        return ESP_FAIL;
    }
    set_result(ESP_OK, "paused");
    return ESP_OK;
}

// --- worker ---

static void worker_task(void *) {
    for (;;) {
        Job job;
        if (xQueueReceive(s_jobs, &job, portMAX_DELAY) != pdTRUE) continue;
        switch (job) {
            case Job::PREPARE:
                prepare(settings::get());
                break;
            case Job::START_ALARM_TRACK:
                start_alarm_track(settings::get());
                break;
            case Job::PAUSE:
                pause();
                break;
        }
    }
}

esp_err_t init() {
    s_jobs = xQueueCreate(4, sizeof(Job));
    ESP_RETURN_ON_FALSE(s_jobs, ESP_ERR_NO_MEM, TAG, "queue");
    BaseType_t ok = xTaskCreate(worker_task, "spotify_api", TASK_STACK, nullptr, 5, nullptr);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "task");
    // New credentials invalidate the cached access token.
    settings::on_change([](const settings::Config &old_cfg, const settings::Config &cfg) {
        if (cfg.spotify_client_id != old_cfg.spotify_client_id ||
            cfg.spotify_client_secret != old_cfg.spotify_client_secret) {
            std::lock_guard<std::mutex> lock(s_mutex);
            s_token_expires_us = 0;
        }
    });
    return ESP_OK;
}

void prepare_async() {
    const Job job = Job::PREPARE;
    if (xQueueSend(s_jobs, &job, 0) != pdTRUE) ESP_LOGW(TAG, "busy, request dropped");
}

PrepareResult prepare_result() {
    std::lock_guard<std::mutex> lock(s_mutex);
    return s_prepare_result;
}

void start_alarm_track_async() {
    const Job job = Job::START_ALARM_TRACK;
    if (xQueueSend(s_jobs, &job, 0) != pdTRUE) ESP_LOGW(TAG, "busy, request dropped");
}

void pause_async() {
    const Job job = Job::PAUSE;
    if (xQueueSend(s_jobs, &job, 0) != pdTRUE) ESP_LOGW(TAG, "busy, request dropped");
}

std::string last_result() {
    std::lock_guard<std::mutex> lock(s_mutex);
    return s_last_result;
}

}  // namespace spotify_api

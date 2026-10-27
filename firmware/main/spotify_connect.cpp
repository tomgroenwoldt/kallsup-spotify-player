#include "spotify_connect.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <map>
#include <memory>
#include <mutex>
#include <string_view>

#include "BellLogger.h"
#include "CSpotContext.h"
#include "LoginBlob.h"
#include "SpircHandler.h"
#include "TrackPlayer.h"
#include "audio_out.h"
#include "esp_check.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mdns.h"
#include "net.h"
#include "nvs.h"
#include "settings.h"
#include "web_ui.h"

namespace spotify_connect {

static const char *TAG = "spotify";

static const char *NVS_NAMESPACE = "spotify";
static const char *NVS_KEY_CREDS = "creds";  // cspot credentials JSON (reusable auth, secret)

constexpr size_t SESSION_STACK = 32 * 1024;  // in PSRAM, like cspot's own tasks
constexpr TickType_t RETRY_DELAY = pdMS_TO_TICKS(5000);
constexpr size_t MAX_ZEROCONF_BODY = 4096;
constexpr int VOLUME_MAX = 65535;
constexpr int VOLUME_STEP = (VOLUME_MAX + 1) / 16;
// Button volume steps reach Spotify once they pause this long: a burst of state
// updates while a button is held has made Spotify move playback to the phone.
constexpr int64_t VOLUME_REPORT_DELAY_US = 500 * 1000;

using EventType = cspot::SpircHandler::EventType;

// --- shared state ---

static std::shared_ptr<cspot::LoginBlob> s_zeroconf_blob;  // its DH key is published via getInfo
static SemaphoreHandle_t s_zeroconf_ready;
static std::mutex s_zeroconf_mutex;
static std::map<std::string, std::string> s_zeroconf_query;
static std::atomic<bool> s_zeroconf_waiting{false};  // logins are only taken while true

static std::atomic<bool> s_logged_in{false};
static std::atomic<bool> s_active{false};
static std::atomic<bool> s_remote_paused{false};
static std::atomic<bool> s_alarm_mode{false};
static std::atomic<int> s_volume{VOLUME_MAX / 2};
static std::mutex s_user_mutex;
static std::string s_user;

// The session task's stack is in PSRAM, where flash access (NVS) is not
// allowed. Credential writes therefore run in the esp_timer task.
static std::mutex s_creds_mutex;
static std::string s_creds_stored;   // what NVS holds
static std::string s_creds_pending;  // what should be written next
static esp_timer_handle_t s_save_timer;

// --- playback tracking (session task + cspot's player task) ---
//
// The ring buffer holds up to 2 s, so the decoder runs ahead of what is heard.
// Spotify is told about a track change only once its first frame was played.
static std::shared_ptr<cspot::SpircHandler> s_handler;
static std::mutex s_track_mutex;
static std::string s_play_track;    // track being heard
static std::string s_stream_track;  // track being decoded into the buffer
static std::atomic<uint64_t> s_mark_frame{0};
static std::atomic<bool> s_mark_pending{false};
static std::atomic<bool> s_depleted{false};
static std::atomic<bool> s_flushed{false};
static std::atomic<bool> s_notify_next{true};
static std::atomic<int> s_start_offset_ms{0};
// TRACK_INFO arrives while poll_playback() holds s_track_mutex, hence its own mutex.
static std::mutex s_now_playing_mutex;
static Track s_now_playing;

// Button commands, run by the session task: cspot isn't thread-safe.
static QueueHandle_t s_media_queue;
static std::atomic<int64_t> s_volume_report_at{0};  // esp_timer time; 0 if nothing to report

// --- credentials ---

static void on_save_timer(void *) {
    std::string creds;
    {
        std::lock_guard<std::mutex> lock(s_creds_mutex);
        if (s_creds_pending == s_creds_stored) return;
        creds = s_creds_pending;
    }
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) return;
    esp_err_t err = creds.empty() ? nvs_erase_key(nvs, NVS_KEY_CREDS) : nvs_set_str(nvs, NVS_KEY_CREDS, creds.c_str());
    if (err == ESP_OK || err == ESP_ERR_NVS_NOT_FOUND) err = nvs_commit(nvs);
    nvs_close(nvs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "storing credentials failed: %s", esp_err_to_name(err));
        return;
    }
    std::lock_guard<std::mutex> lock(s_creds_mutex);
    s_creds_stored = creds;
    ESP_LOGI(TAG, "%s", creds.empty() ? "stored credentials erased" : "credentials stored");
}

static void save_credentials(const std::string &creds) {
    {
        std::lock_guard<std::mutex> lock(s_creds_mutex);
        s_creds_pending = creds;
    }
    esp_timer_stop(s_save_timer);
    esp_timer_start_once(s_save_timer, 100 * 1000);
}

static std::string load_credentials() {
    nvs_handle_t nvs;
    std::string creds;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) return creds;
    size_t len = 0;
    if (nvs_get_str(nvs, NVS_KEY_CREDS, nullptr, &len) == ESP_OK && len > 1) {
        creds.resize(len);
        if (nvs_get_str(nvs, NVS_KEY_CREDS, creds.data(), &len) == ESP_OK) {
            creds.resize(len - 1);
        } else {
            creds.clear();
        }
    }
    nvs_close(nvs);
    return creds;
}

// --- volume ---

static void apply_volume() {
    if (s_alarm_mode) return;
    const float v = s_volume / 65535.0f;
    audio_out::set_gain(v * v * settings::get().max_gain);
}

// --- zeroconf (runs in the HTTP server task) ---

static std::string url_decode(std::string_view in) {
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); i++) {
        if (in[i] == '+') {
            out += ' ';
        } else if (in[i] == '%' && i + 2 < in.size() && isxdigit((unsigned char)in[i + 1]) &&
                   isxdigit((unsigned char)in[i + 2])) {
            out += (char)strtol(std::string(in.substr(i + 1, 2)).c_str(), nullptr, 16);
            i += 2;
        } else {
            out += in[i];
        }
    }
    return out;
}

static std::map<std::string, std::string> parse_form(std::string_view body) {
    std::map<std::string, std::string> out;
    while (!body.empty()) {
        const size_t amp = body.find('&');
        const std::string_view pair = body.substr(0, amp);
        const size_t eq = pair.find('=');
        if (eq != std::string_view::npos) out[url_decode(pair.substr(0, eq))] = url_decode(pair.substr(eq + 1));
        if (amp == std::string_view::npos) break;
        body.remove_prefix(amp + 1);
    }
    return out;
}

static esp_err_t send_json(httpd_req_t *req, const std::string &json) {
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json.data(), json.size());
}

static esp_err_t get_spotify_info(httpd_req_t *req) {
    return send_json(req, s_zeroconf_blob->buildZeroconfInfo());
}

static esp_err_t post_spotify_info(httpd_req_t *req) {
    if (req->content_len == 0 || req->content_len > MAX_ZEROCONF_BODY) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body");
    }
    std::string body(req->content_len, '\0');
    size_t got = 0;
    while (got < body.size()) {
        const int r = httpd_req_recv(req, body.data() + got, body.size() - got);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (r <= 0) return ESP_FAIL;
        got += r;
    }
    auto query = parse_form(body);
    // Only while waiting to be paired: once logged in, someone else on the network mustn't take over.
    if (s_zeroconf_waiting && query["action"] == "addUser" && !query["blob"].empty() &&
        !query["clientKey"].empty() && !query["userName"].empty()) {
        ESP_LOGI(TAG, "zeroconf login from the Spotify app");
        {
            std::lock_guard<std::mutex> lock(s_zeroconf_mutex);
            s_zeroconf_query = std::move(query);
        }
        xSemaphoreGive(s_zeroconf_ready);
    }
    return send_json(req, R"({"status":101,"spotifyError":0,"statusString":"ERROR-OK"})");
}

static void advertise_zeroconf(bool on) {
    if (on) {
        mdns_txt_item_t txt[] = {{"VERSION", "1.0"}, {"CPath", "/spotify_info"}, {"Stack", "SP"}};
        const std::string name = settings::get().device_name;
        if (mdns_service_add(name.c_str(), "_spotify-connect", "_tcp", 80, txt, 3) == ESP_OK) {
            ESP_LOGI(TAG, "waiting for the Spotify app: choose \"%s\" under Connect to a device", name.c_str());
        }
    } else {
        mdns_service_remove("_spotify-connect", "_tcp");
    }
}

// --- cspot callbacks ---

static void forget_streaming_track() {
    s_mark_pending = false;
    std::lock_guard<std::mutex> lock(s_track_mutex);
    s_stream_track.clear();
}

// cspot's player task hands over decoded PCM. Returning less than `bytes`
// (even 0) makes it retry after 50 ms; that's how backpressure and pause work.
static size_t on_pcm(uint8_t *pcm, size_t bytes, std::string_view track_id) {
    {
        std::lock_guard<std::mutex> lock(s_track_mutex);
        if (s_stream_track != track_id) {
            // At most one track ahead of what is heard.
            if (s_play_track != track_id && s_play_track != s_stream_track) return 0;
            s_stream_track = std::string(track_id);
            s_mark_frame = audio_out::stats().frames_written;
            s_mark_pending = true;
        }
    }
    if (bytes < 4) return bytes;  // no whole stereo frame: drop the remainder
    const size_t frames = audio_out::write_pcm(reinterpret_cast<const int16_t *>(pcm), bytes / 4, 0);
    return frames * 4;
}

static void on_event(std::unique_ptr<cspot::SpircHandler::Event> event) {
    switch (event->eventType) {
        case EventType::PLAYBACK_START: {
            {
                std::lock_guard<std::mutex> lock(s_track_mutex);
                s_play_track.clear();
                s_stream_track.clear();
            }
            s_mark_pending = false;
            s_depleted = false;
            s_start_offset_ms = std::get<int>(event->data);
            s_notify_next = !s_flushed.exchange(false);
            audio_out::flush();
            audio_out::set_paused(false);
            s_remote_paused = false;
            s_active = true;
            // Spotify doesn't send the volume when a device becomes active.
            if (s_handler) s_handler->setRemoteVolume(s_volume);
            apply_volume();
            ESP_LOGI(TAG, "playback start at %d ms", s_start_offset_ms.load());
            break;
        }
        case EventType::PLAY_PAUSE: {
            const bool paused = std::get<bool>(event->data);
            audio_out::set_paused(paused);
            s_remote_paused = paused;
            ESP_LOGI(TAG, "%s", paused ? "paused" : "resumed");
            break;
        }
        case EventType::TRACK_INFO: {
            const auto &info = std::get<cspot::TrackInfo>(event->data);
            ESP_LOGI(TAG, "now playing: %s - %s (%lu s)", info.artist.c_str(), info.name.c_str(),
                     (unsigned long)(info.duration / 1000));
            std::lock_guard<std::mutex> lock(s_now_playing_mutex);
            s_now_playing = {info.name, info.artist};
            break;
        }
        case EventType::FLUSH:
            // Queue replaced; the next PLAYBACK_START must not advance the queue again.
            s_flushed = true;
            [[fallthrough]];
        case EventType::NEXT:
        case EventType::PREV:
            forget_streaming_track();
            audio_out::flush();
            break;
        case EventType::SEEK:
            audio_out::flush();
            break;
        case EventType::DEPLETED:
            s_depleted = true;  // poll_playback() reports the end once it was heard
            break;
        case EventType::VOLUME:
            s_volume = std::get<int>(event->data);
            s_volume_report_at = 0;  // Spotify's value wins over an unreported step
            apply_volume();
            break;
        case EventType::DISC:
            ESP_LOGI(TAG, "another device took over");
            audio_out::flush();
            audio_out::set_paused(false);
            s_mark_pending = false;
            s_depleted = false;
            s_active = false;
            {
                std::lock_guard<std::mutex> lock(s_now_playing_mutex);
                s_now_playing = {};
            }
            break;
    }
}

// Session task, after every packet (at least 5x per second).
static void poll_playback(cspot::SpircHandler &handler) {
    if (!s_mark_pending && !s_depleted) return;
    const audio_out::Stats st = audio_out::stats();
    if (s_mark_pending && st.frames_played + st.frames_dropped >= s_mark_frame) {
        s_mark_pending = false;
        {
            std::lock_guard<std::mutex> lock(s_track_mutex);
            s_play_track = s_stream_track;
            // Tells Spotify (and cspot's queue) that this track is now audible.
            if (s_notify_next) {
                handler.notifyAudioReachedPlayback(s_play_track);
            } else {
                s_notify_next = true;
            }
        }
        handler.updatePositionMs(s_start_offset_ms.exchange(0));
    }
    if (s_depleted && !s_mark_pending && st.frames_queued == 0) {
        s_depleted = false;
        ESP_LOGI(TAG, "end of queue");
        handler.notifyAudioEnded();
    }
}

static void run_media(cspot::SpircHandler &handler, Media cmd) {
    if (!s_active) {
        ESP_LOGI(TAG, "not the active player, ignoring media button");
        return;
    }
    switch (cmd) {
        case Media::PLAY_PAUSE:
            handler.setPause(!s_remote_paused);  // reports back as PLAY_PAUSE
            break;
        case Media::NEXT:
        case Media::PREVIOUS:
            // What on_event does for NEXT/PREV; cspot only reports remote skips.
            if (cmd == Media::NEXT ? handler.nextSong() : handler.previousSong()) {
                forget_streaming_track();
                audio_out::flush();
            }
            break;
        case Media::VOLUME_UP:
        case Media::VOLUME_DOWN: {
            const int step = cmd == Media::VOLUME_UP ? VOLUME_STEP : -VOLUME_STEP;
            const int volume = std::clamp(s_volume + step, 0, VOLUME_MAX);
            if (volume == s_volume) break;
            s_volume = volume;
            apply_volume();
            s_volume_report_at = esp_timer_get_time() + VOLUME_REPORT_DELAY_US;
            ESP_LOGI(TAG, "volume %d%%", volume * 100 / VOLUME_MAX);
            break;
        }
    }
}

// --- session ---

enum class LoginResult { DECLINED, FAILED };

// Logs in and runs the session. Returns only if the login didn't succeed.
static LoginResult run_session(std::shared_ptr<cspot::LoginBlob> blob) {
    const settings::Config cfg = settings::get();
    if (cfg.spotify_client_id.empty() || cfg.spotify_client_secret.empty()) {
        ESP_LOGW(TAG, "Spotify client ID/secret not set: login works, but playback will fail");
    }
    auto ctx = cspot::Context::createFromBlob(blob);
    ctx->config.clientId = cfg.spotify_client_id;
    ctx->config.clientSecret = cfg.spotify_client_secret;
    ctx->config.audioFormat = AudioFormat_OGG_VORBIS_160;

    ctx->session->connectWithRandomAp();
    const std::vector<uint8_t> reusable = ctx->session->authenticate(blob);
    if (reusable.empty()) return LoginResult::DECLINED;

    // Store the reusable credentials Spotify just issued; they replace the
    // one-time zeroconf blob and are refreshed on every login.
    ctx->config.authData = reusable;
    save_credentials(ctx->getCredentialsJson());
    {
        std::lock_guard<std::mutex> lock(s_user_mutex);
        s_user = ctx->config.username;
    }
    ESP_LOGI(TAG, "logged in as %s", ctx->config.username.c_str());

    ctx->session->startTask();
    auto handler = std::make_shared<cspot::SpircHandler>(ctx);
    handler->getTrackPlayer()->setDataCallback(on_pcm);
    handler->setEventHandler(on_event);
    s_handler = handler;
    handler->subscribeToMercury();
    s_logged_in = true;
    advertise_zeroconf(false);

    // cspot reconnects by itself; this loop only ends through an exception.
    try {
        for (;;) {
            ctx->session->handlePacket();
            poll_playback(*handler);
            Media cmd;
            while (xQueueReceive(s_media_queue, &cmd, 0) == pdTRUE) run_media(*handler, cmd);
            const int64_t report_at = s_volume_report_at;
            if (report_at != 0 && esp_timer_get_time() >= report_at) {
                s_volume_report_at = 0;
                handler->setRemoteVolume(s_volume);
            }
        }
    } catch (const std::exception &e) {
        // cspot's tasks can't be torn down safely while running; restart cleanly.
        ESP_LOGE(TAG, "session failed (%s), restarting", e.what());
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
    }
    return LoginResult::FAILED;
}

static void session_task(void *arg) {
    std::string stored = *static_cast<std::string *>(arg);
    delete static_cast<std::string *>(arg);

    for (;;) {
        // Login and certificate checks need the network and a correct clock.
        while (!net::status().sta_connected || !net::time_synced()) vTaskDelay(pdMS_TO_TICKS(500));

        std::shared_ptr<cspot::LoginBlob> blob;
        const bool from_stored = !stored.empty();
        if (from_stored) {
            blob = std::make_shared<cspot::LoginBlob>(settings::get().device_name);
            try {
                blob->loadJson(stored);
            } catch (const std::exception &) {
                blob = nullptr;
            }
            if (!blob || blob->username.empty() || blob->authData.empty()) {
                ESP_LOGW(TAG, "stored credentials unreadable, waiting for the app instead");
                stored.clear();
                save_credentials("");
                continue;
            }
            ESP_LOGI(TAG, "logging in with stored credentials of %s", blob->username.c_str());
        } else {
            xSemaphoreTake(s_zeroconf_ready, 0);  // drop a login that came in before
            s_zeroconf_waiting = true;
            advertise_zeroconf(true);
            xSemaphoreTake(s_zeroconf_ready, portMAX_DELAY);
            s_zeroconf_waiting = false;
            std::map<std::string, std::string> query;
            {
                std::lock_guard<std::mutex> lock(s_zeroconf_mutex);
                query = std::move(s_zeroconf_query);
            }
            blob = s_zeroconf_blob;
            try {
                blob->loadZeroconfQuery(query);
            } catch (const std::exception &e) {
                ESP_LOGE(TAG, "zeroconf blob rejected: %s", e.what());
                continue;
            }
        }

        LoginResult result;
        try {
            result = run_session(blob);
        } catch (const std::exception &e) {
            ESP_LOGE(TAG, "login failed: %s", e.what());
            result = LoginResult::FAILED;
        }
        if (result == LoginResult::DECLINED) {
            ESP_LOGE(TAG, "Spotify declined the login%s", from_stored ? "; forgetting stored credentials" : "");
            if (from_stored) {
                stored.clear();
                save_credentials("");
            }
        }
        vTaskDelay(RETRY_DELAY);
    }
}

// --- public ---

esp_err_t init() {
    bell::setDefaultLogger();  // cspot logs through this; it must exist first

    esp_timer_create_args_t timer_args = {};
    timer_args.callback = on_save_timer;
    timer_args.name = "spotify_creds";
    ESP_RETURN_ON_ERROR(esp_timer_create(&timer_args, &s_save_timer), TAG, "timer");

    s_zeroconf_ready = xSemaphoreCreateBinary();
    ESP_RETURN_ON_FALSE(s_zeroconf_ready, ESP_ERR_NO_MEM, TAG, "semaphore");
    s_media_queue = xQueueCreate(4, sizeof(Media));
    ESP_RETURN_ON_FALSE(s_media_queue, ESP_ERR_NO_MEM, TAG, "media queue");
    s_zeroconf_blob = std::make_shared<cspot::LoginBlob>(settings::get().device_name);

    httpd_uri_t get_info = {};
    get_info.uri = "/spotify_info";
    get_info.method = HTTP_GET;
    get_info.handler = get_spotify_info;
    ESP_RETURN_ON_ERROR(web_ui::register_route(get_info), TAG, "GET /spotify_info");
    httpd_uri_t add_user = {};
    add_user.uri = "/spotify_info";
    add_user.method = HTTP_POST;
    add_user.handler = post_spotify_info;
    ESP_RETURN_ON_ERROR(web_ui::register_route(add_user), TAG, "POST /spotify_info");

    settings::on_change([](const settings::Config &old_cfg, const settings::Config &cfg) {
        if (cfg.max_gain != old_cfg.max_gain) apply_volume();
    });

    // Read NVS here: the session task's PSRAM stack can't access flash.
    auto *stored = new std::string(load_credentials());
    s_creds_stored = *stored;
    ESP_LOGI(TAG, "stored credentials: %s", stored->empty() ? "none" : "present");

    TaskHandle_t task;
    BaseType_t ok = xTaskCreatePinnedToCoreWithCaps(session_task, "spotify", SESSION_STACK, stored, 5, &task, 1,
                                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ok != pdPASS) {
        delete stored;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

bool logged_in() {
    return s_logged_in;
}

std::string user_name() {
    std::lock_guard<std::mutex> lock(s_user_mutex);
    return s_user;
}

bool active() {
    return s_active;
}

bool remote_paused() {
    return s_remote_paused;
}

Track now_playing() {
    std::lock_guard<std::mutex> lock(s_now_playing_mutex);
    return s_now_playing;
}

void media(Media cmd) {
    if (!s_logged_in || xQueueSend(s_media_queue, &cmd, 0) != pdTRUE) {
        ESP_LOGW(TAG, "media button dropped");
    }
}

void set_alarm_mode(bool on) {
    s_alarm_mode = on;
    if (!on) apply_volume();
}

void forget_credentials() {
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_erase_key(nvs, NVS_KEY_CREDS);
        nvs_commit(nvs);
        nvs_close(nvs);
    }
    ESP_LOGW(TAG, "stored credentials erased, restarting");
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_restart();
}

}  // namespace spotify_connect

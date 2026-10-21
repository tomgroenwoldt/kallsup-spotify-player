#include "alarm.h"

#include <atomic>

#include "audio_out.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "net.h"
#include "spotify_api.h"
#include "spotify_connect.h"

namespace alarm_clock {

static const char *TAG = "alarm";

constexpr time_t PREPARE_S = 90;
constexpr time_t SPOTIFY_TIMEOUT_S = 15;  // no (more) Spotify PCM -> fallback beep
constexpr int DAILY_CHECK_HOUR = 4;       // local time of the daily session check
constexpr time_t DAILY_CHECK_TIMEOUT_S = 60;
constexpr time_t HEAP_LOG_S = 60;
constexpr TickType_t TICK = pdMS_TO_TICKS(500);
// Four short beeps, then a pause; runs until stopped.
constexpr audio_out::Pattern FALLBACK_BEEP = {1000.0f, 120, 80, 4, 700, 0};

enum class Cmd { RING, RING_FALLBACK, SNOOZE, STOP };

static QueueHandle_t s_cmds;
static std::atomic<State> s_state{State::IDLE};
static std::atomic<time_t> s_snooze_until{0};
static time_t s_target = 0;        // alarm time while PREPARING
static time_t s_ring_started = 0;  // for auto-stop

// Spotify watchdog, while RINGING_SPOTIFY
static bool s_spotify_requested = false;  // since the last silence(): Spotify may still start
static bool s_got_audio = false;
static uint64_t s_last_frames = 0;
static time_t s_last_progress = 0;

// Daily session check
static int s_last_check_yday = -1;
static uint32_t s_check_seq = 0;
static time_t s_check_deadline = 0;

static void set_state(State st) {
    const State old_st = s_state.exchange(st);
    if (old_st != st) ESP_LOGI(TAG, "%s -> %s", state_name(old_st), state_name(st));
}

static bool api_configured(const settings::Config &cfg) {
    return !cfg.spotify_client_id.empty() && !cfg.spotify_client_secret.empty() &&
           !cfg.spotify_refresh_token.empty();
}

// Why Spotify can't be used right now, or nullptr if it can.
static const char *spotify_unusable(const settings::Config &cfg) {
    if (cfg.track_uri.empty()) return "no track set";
    if (!api_configured(cfg)) return "Web API not set up";
    if (!net::status().sta_connected) return "no Wi-Fi";
    if (!spotify_connect::logged_in()) return "Spotify session not logged in";
    return nullptr;
}

static void ring_fallback(const settings::Config &cfg) {
    if (s_spotify_requested) {
        // Spotify might still start late: hold its PCM and pause it.
        audio_out::set_paused(true);
        spotify_api::pause_async();
    }
    audio_out::fade_in(1.0f, cfg.fade_in_s * 1000);  // capped at max_gain
    audio_out::play_pattern(FALLBACK_BEEP);
    set_state(State::RINGING_FALLBACK);
}

static void start_ringing(const settings::Config &cfg, time_t now, bool fallback_only) {
    s_ring_started = now;
    s_snooze_until = 0;
    const char *why_not = fallback_only ? "fallback requested" : spotify_unusable(cfg);
    if (why_not) {
        ESP_LOGW(TAG, "ringing with the fallback beep: %s", why_not);
        ring_fallback(cfg);
        return;
    }
    // Alarm mode: Spotify's volume is ignored, the gain fades in locally.
    spotify_connect::set_alarm_mode(true);
    audio_out::fade_in(1.0f, cfg.fade_in_s * 1000);
    spotify_api::start_alarm_track_async();
    s_spotify_requested = true;
    s_got_audio = false;
    s_last_frames = audio_out::stats().frames_written;
    s_last_progress = now;
    set_state(State::RINGING_SPOTIFY);
}

static void silence() {
    if (s_spotify_requested) {
        audio_out::set_paused(true);  // hold cspot's PCM until Spotify's pause lands
        spotify_api::pause_async();
        s_spotify_requested = false;
    }
    audio_out::stop();
}

static void go_idle() {
    s_snooze_until = 0;
    spotify_connect::set_alarm_mode(false);
    set_state(State::IDLE);
}

static void handle(Cmd cmd, const settings::Config &cfg, time_t now) {
    switch (cmd) {
        case Cmd::RING:
        case Cmd::RING_FALLBACK:
            if (ringing()) silence();
            start_ringing(cfg, now, cmd == Cmd::RING_FALLBACK);
            break;
        case Cmd::SNOOZE:
            if (!ringing()) break;
            silence();
            s_snooze_until = now + cfg.snooze_min * 60;
            set_state(State::SNOOZED);
            ESP_LOGI(TAG, "snoozed for %d min", cfg.snooze_min);
            break;
        case Cmd::STOP:
            silence();
            if (ringing() || s_state == State::SNOOZED) go_idle();
            break;
    }
}

// Spotify must deliver PCM within 15 s of the play request, and keep doing so.
static void watch_spotify(const settings::Config &cfg, time_t now) {
    const uint64_t frames = audio_out::stats().frames_written;
    if (frames != s_last_frames) {
        s_last_frames = frames;
        s_last_progress = now;
        s_got_audio = true;
    }
    if (s_got_audio && (spotify_connect::remote_paused() || !spotify_connect::active())) {
        // Paused or taken over in the Spotify app: the user is awake.
        ESP_LOGI(TAG, "stopped from the Spotify app");
        s_spotify_requested = false;  // already paused remotely
        audio_out::stop();
        go_idle();
        return;
    }
    if (now - s_last_progress >= SPOTIFY_TIMEOUT_S) {
        ESP_LOGW(TAG, "%s, switching to the fallback beep",
                 s_got_audio ? "Spotify audio stalled" : "no Spotify audio within 15 s");
        ring_fallback(cfg);
    }
}

// Once a day, while idle: is this device still visible to Spotify? A session
// that looks logged in but isn't listed is stale; a restart logs in afresh.
static void daily_check(const settings::Config &cfg, time_t now) {
    if (s_check_deadline != 0) {
        const spotify_api::PrepareResult r = spotify_api::prepare_result();
        if (r.seq != s_check_seq) {
            s_check_deadline = 0;
            if (r.err == ESP_ERR_NOT_FOUND) {
                ESP_LOGW(TAG, "daily check: device not listed by Spotify, restarting the session");
                vTaskDelay(pdMS_TO_TICKS(200));
                esp_restart();
            }
            ESP_LOGI(TAG, "daily check: %s", r.err == ESP_OK ? "ok" : esp_err_to_name(r.err));
        } else if (now > s_check_deadline) {
            s_check_deadline = 0;
            ESP_LOGW(TAG, "daily check: no answer");
        }
        return;
    }
    struct tm tm;
    localtime_r(&now, &tm);
    if (tm.tm_hour != DAILY_CHECK_HOUR || tm.tm_yday == s_last_check_yday) return;
    s_last_check_yday = tm.tm_yday;
    if (!api_configured(cfg) || !net::status().sta_connected) return;
    if (!spotify_connect::logged_in()) {
        ESP_LOGW(TAG, "daily check: Spotify session not logged in, restarting");
        vTaskDelay(pdMS_TO_TICKS(200));
        esp_restart();
    }
    s_check_seq = spotify_api::prepare_result().seq;
    s_check_deadline = now + DAILY_CHECK_TIMEOUT_S;
    spotify_api::prepare_async();
}

static void tick(const settings::Config &cfg, time_t now) {
    switch (s_state.load()) {
        case State::IDLE: {
            if (!time_ok()) break;
            const time_t next = next_alarm(cfg, now);
            if (next != 0 && next - now <= PREPARE_S) {
                s_target = next;
                set_state(State::PREPARING);
                // Section 4.1: Wi-Fi up, session logged in, token fresh, device id known.
                if (!net::status().sta_connected) net::reconnect();
                if (!spotify_connect::logged_in()) ESP_LOGW(TAG, "Spotify session not logged in");
                if (!spotify_unusable(cfg)) spotify_api::prepare_async();
            } else {
                daily_check(cfg, now);
            }
            break;
        }
        case State::PREPARING:
            if (now >= s_target) {
                start_ringing(cfg, now, false);
            } else if (next_alarm(cfg, now) != s_target) {
                go_idle();  // alarm changed or disabled meanwhile
            }
            break;
        case State::RINGING_SPOTIFY:
        case State::RINGING_FALLBACK:
            if (now - s_ring_started >= (time_t)cfg.auto_stop_min * 60) {
                ESP_LOGI(TAG, "auto-stop after %d min", cfg.auto_stop_min);
                silence();
                go_idle();
            } else if (s_state == State::RINGING_SPOTIFY) {
                watch_spotify(cfg, now);
            }
            break;
        case State::SNOOZED:
            if (now >= s_snooze_until) start_ringing(cfg, now, false);
            break;
    }
}

static void alarm_task(void *) {
    // A hung alarm task means no alarm: let the task watchdog reboot instead.
    ESP_ERROR_CHECK(esp_task_wdt_add(nullptr));
    time_t last_heap_log = 0;
    for (;;) {
        Cmd cmd;
        const bool got = xQueueReceive(s_cmds, &cmd, TICK) == pdTRUE;
        esp_task_wdt_reset();
        const settings::Config cfg = settings::get();
        const time_t now = time(nullptr);
        if (got) handle(cmd, cfg, now);
        tick(cfg, now);
        if (now - last_heap_log >= HEAP_LOG_S) {
            last_heap_log = now;
            ESP_LOGI(TAG, "heap: internal %u (min %u), PSRAM %u",
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                     (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        }
    }
}

static void send(Cmd cmd) {
    xQueueSend(s_cmds, &cmd, 0);
}

esp_err_t init() {
    s_cmds = xQueueCreate(8, sizeof(Cmd));
    ESP_RETURN_ON_FALSE(s_cmds, ESP_ERR_NO_MEM, TAG, "queue");
    BaseType_t ok = xTaskCreate(alarm_task, "alarm", 4096, nullptr, 10, nullptr);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "task");
    return ESP_OK;
}

void ring_now() {
    send(Cmd::RING);
}

void ring_fallback() {
    send(Cmd::RING_FALLBACK);
}

void snooze() {
    send(Cmd::SNOOZE);
}

void stop() {
    send(Cmd::STOP);
}

State state() {
    return s_state;
}

const char *state_name(State state) {
    switch (state) {
        case State::IDLE: return "idle";
        case State::PREPARING: return "preparing";
        case State::RINGING_SPOTIFY: return "ringing_spotify";
        case State::RINGING_FALLBACK: return "ringing_fallback";
        case State::SNOOZED: return "snoozed";
    }
    return "?";
}

bool ringing() {
    const State st = s_state;
    return st == State::RINGING_SPOTIFY || st == State::RINGING_FALLBACK;
}

time_t snoozed_until() {
    return s_snooze_until;
}

bool time_ok() {
    return net::time_synced();
}

time_t next_alarm(const settings::Config &cfg, time_t now) {
    struct tm today;
    localtime_r(&now, &today);
    time_t best = 0;
    // Today plus 7 days covers every weekday, including today's weekday next week.
    for (int offset = 0; offset <= 7; offset++) {
        for (const settings::Alarm &a : cfg.alarms) {
            if (!a.enabled) continue;
            struct tm t = today;
            t.tm_mday += offset;
            t.tm_hour = a.hour;
            t.tm_min = a.minute;
            t.tm_sec = 0;
            t.tm_isdst = -1;  // let mktime work out DST for that day
            const time_t when = mktime(&t);  // also normalises tm_wday
            if (when <= now || !(a.weekdays & (1 << t.tm_wday))) continue;
            if (best == 0 || when < best) best = when;
        }
        if (best != 0) break;
    }
    return best;
}

}  // namespace alarm_clock

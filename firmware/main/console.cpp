#include "console.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include "alarm.h"
#include "audio_out.h"
#include "esp_check.h"
#include "esp_console.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "net.h"
#include "settings.h"
#include "spotify_connect.h"

namespace console {

static const char *TAG = "console";

// tone [freq_hz] [duration_ms] [gain] — defaults: 440 Hz, 3 s, 30 %
static int cmd_tone(int argc, char **argv) {
    const float freq = argc > 1 ? strtof(argv[1], nullptr) : 440.0f;
    const uint32_t ms = argc > 2 ? strtoul(argv[2], nullptr, 10) : 3000;
    const float gain = argc > 3 ? strtof(argv[3], nullptr) : 0.3f;
    audio_out::set_gain(gain);
    audio_out::play_tone(freq, ms);
    printf("tone %.0f Hz, %lu ms, gain %.2f (max_gain %.2f)\n", freq, (unsigned long)ms, gain,
           audio_out::max_gain());
    return 0;
}

static int cmd_stop(int, char **) {
    alarm_clock::stop();
    return 0;
}

static int cmd_ring(int, char **) {
    alarm_clock::ring_now();
    return 0;
}

static int cmd_snooze(int, char **) {
    alarm_clock::snooze();
    return 0;
}

static int cmd_alarm(int, char **) {
    const settings::Config cfg = settings::get();
    const time_t now = time(nullptr);
    printf("state %s\n", alarm_clock::state_name(alarm_clock::state()));
    if (const time_t until = alarm_clock::snoozed_until()) {
        char buf[16];
        struct tm tm;
        localtime_r(&until, &tm);
        strftime(buf, sizeof(buf), "%H:%M:%S", &tm);
        printf("snoozed until %s\n", buf);
    }
    if (!alarm_clock::time_ok()) {
        printf("time not synced: alarms can't fire\n");
        return 0;
    }
    const time_t next = alarm_clock::next_alarm(cfg, now);
    if (next) {
        char buf[32];
        struct tm tm;
        localtime_r(&next, &tm);
        strftime(buf, sizeof(buf), "%a %Y-%m-%d %H:%M", &tm);
        printf("next alarm %s (in %ld s)\n", buf, (long)(next - now));
    } else {
        printf("no alarm enabled\n");
    }
    return 0;
}

// max_gain [value]
static int cmd_max_gain(int argc, char **argv) {
    if (argc > 1) audio_out::set_max_gain(strtof(argv[1], nullptr));
    printf("max_gain %.2f\n", audio_out::max_gain());
    return 0;
}

static int cmd_heap(int, char **) {
    printf("free internal %u, PSRAM %u, min internal ever %u\n",
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
           (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
    return 0;
}

// wifi                     — show status
// wifi <ssid> [password]   — store credentials and reconnect (quote values with spaces)
static int cmd_wifi(int argc, char **argv) {
    if (argc > 1) {
        settings::Config cfg = settings::get();
        cfg.wifi_ssid = argv[1];
        cfg.wifi_pass = argc > 2 ? argv[2] : "";
        std::string error;
        if (settings::save(cfg, &error) != ESP_OK) {
            printf("not saved: %s\n", error.c_str());
            return 1;
        }
        printf("saved Wi-Fi \"%s\" (%s), reconnecting\n", argv[1], argc > 2 ? "with password" : "open");
        return 0;
    }
    const net::Status st = net::status();
    if (st.sta_connected) {
        printf("connected to \"%s\", IP %s, RSSI %d dBm, http://%s.local\n", st.ssid.c_str(), st.ip.c_str(),
               st.rssi, net::HOSTNAME);
    } else {
        const std::string ssid = settings::get().wifi_ssid;
        printf("not connected (stored SSID: %s)\n", ssid.empty() ? "none" : ssid.c_str());
    }
    printf("setup AP \"%s\": %s\n", net::AP_SSID, st.ap_active ? "on" : "off");
    return 0;
}

static int cmd_time(int, char **) {
    const time_t now = time(nullptr);
    struct tm tm;
    localtime_r(&now, &tm);
    char buf[48];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S %Z", &tm);
    printf("%s (%s)\n", buf, net::time_synced() ? "synced" : "NOT synced since boot");
    return 0;
}

// tz [posix_tz]
static int cmd_tz(int argc, char **argv) {
    if (argc > 1) {
        settings::Config cfg = settings::get();
        cfg.time_zone = argv[1];
        std::string error;
        if (settings::save(cfg, &error) != ESP_OK) {
            printf("not saved: %s\n", error.c_str());
            return 1;
        }
    }
    printf("%s\n", settings::get().time_zone.c_str());
    return 0;
}

// spotify          — show status
// spotify forget   — erase the stored login and restart
static int cmd_spotify(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "forget") == 0) {
        spotify_connect::forget_credentials();
        return 0;
    }
    if (spotify_connect::logged_in()) {
        printf("logged in as %s, %s\n", spotify_connect::user_name().c_str(),
               spotify_connect::active() ? "active player" : "not the active player");
    } else {
        printf("not logged in\n");
    }
    return 0;
}

static esp_err_t register_cmd(const char *name, const char *help, esp_console_cmd_func_t func) {
    esp_console_cmd_t cmd = {};
    cmd.command = name;
    cmd.help = help;
    cmd.func = func;
    return esp_console_cmd_register(&cmd);
}

esp_err_t init() {
    esp_console_repl_t *repl = nullptr;
    esp_console_repl_config_t repl_cfg = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_cfg.prompt = "kallsup>";

#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    esp_console_dev_usb_serial_jtag_config_t dev_cfg = ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_console_new_repl_usb_serial_jtag(&dev_cfg, &repl_cfg, &repl), TAG, "repl");
#else
    esp_console_dev_uart_config_t dev_cfg = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_console_new_repl_uart(&dev_cfg, &repl_cfg, &repl), TAG, "repl");
#endif

    ESP_RETURN_ON_ERROR(register_cmd("tone", "Play a sine: tone [freq_hz=440] [duration_ms=3000] [gain=0.3]",
                                     cmd_tone), TAG, "tone");
    ESP_RETURN_ON_ERROR(register_cmd("stop", "Stop the alarm and any audio", cmd_stop), TAG, "stop");
    ESP_RETURN_ON_ERROR(register_cmd("ring", "Ring the alarm now", cmd_ring), TAG, "ring");
    ESP_RETURN_ON_ERROR(register_cmd("snooze", "Snooze a ringing alarm", cmd_snooze), TAG, "snooze");
    ESP_RETURN_ON_ERROR(register_cmd("alarm", "Show alarm state and next alarm", cmd_alarm), TAG, "alarm");
    ESP_RETURN_ON_ERROR(register_cmd("max_gain", "Show or set the gain cap: max_gain [0..1]", cmd_max_gain), TAG,
                        "max_gain");
    ESP_RETURN_ON_ERROR(register_cmd("heap", "Show free heap", cmd_heap), TAG, "heap");
    ESP_RETURN_ON_ERROR(register_cmd("wifi", "Show Wi-Fi status, or store credentials: wifi [ssid] [password]",
                                     cmd_wifi), TAG, "wifi");
    ESP_RETURN_ON_ERROR(register_cmd("time", "Show local time and sync state", cmd_time), TAG, "time");
    ESP_RETURN_ON_ERROR(register_cmd("spotify", "Spotify status; spotify forget: erase login, restart", cmd_spotify),
                        TAG, "spotify");
    ESP_RETURN_ON_ERROR(register_cmd("tz", "Show or set the POSIX time zone: tz [TZ]", cmd_tz), TAG, "tz");
    ESP_RETURN_ON_ERROR(esp_console_register_help_command(), TAG, "help");
    return esp_console_start_repl(repl);
}

}  // namespace console

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_psram.h"
#include "nvs_flash.h"

#include "alarm.h"
#include "audio_out.h"
#include "buttons.h"
#include "console.h"
#include "net.h"
#include "settings.h"
#include "spotify_api.h"
#include "spotify_connect.h"
#include "status_led.h"
#include "web_ui.h"

static const char *TAG = "main";

static void init_nvs() {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition needs erase, reinitialising");
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
}

extern "C" void app_main() {
    ESP_LOGI(TAG, "Kallsup Spotify Alarm Clock booting");

    size_t psram = esp_psram_is_initialized() ? esp_psram_get_size() : 0;
    ESP_LOGI(TAG, "PSRAM: %u bytes (%u MB)", (unsigned)psram, (unsigned)(psram / (1024 * 1024)));
    if (psram < 8 * 1024 * 1024) {
        ESP_LOGE(TAG, "Expected 8 MB PSRAM, check sdkconfig (octal mode) and board variant");
    }
    ESP_LOGI(TAG, "Free heap: internal %u, PSRAM %u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    init_nvs();

    ESP_ERROR_CHECK(settings::init());
    ESP_ERROR_CHECK(audio_out::init());
    audio_out::set_max_gain(settings::get().max_gain);
    settings::on_change([](const settings::Config &old_cfg, const settings::Config &cfg) {
        if (cfg.max_gain != old_cfg.max_gain) audio_out::set_max_gain(cfg.max_gain);
    });
    ESP_ERROR_CHECK(net::init());
    ESP_ERROR_CHECK(web_ui::init());
    ESP_ERROR_CHECK(alarm_clock::init());
    ESP_ERROR_CHECK(buttons::init([](buttons::Button b, int clicks) {
        using spotify_connect::Media;
        const bool alarm_on = alarm_clock::ringing() || alarm_clock::state() == alarm_clock::State::SNOOZED;
        if (b == buttons::Button::STOP) {
            if (alarm_on || !spotify_connect::active()) {
                alarm_clock::stop();  // also ends test beeps; would cut into Spotify's stream
            } else if (clicks >= 2) {
                spotify_connect::media(clicks == 2 ? Media::VOLUME_UP : Media::VOLUME_DOWN);
            }
        } else if (alarm_on) {
            alarm_clock::snooze();  // any number of clicks: a sleepy double press still snoozes
        } else {
            spotify_connect::media(clicks == 1 ? Media::PLAY_PAUSE : clicks == 2 ? Media::NEXT : Media::PREVIOUS);
        }
    }));
    ESP_ERROR_CHECK(spotify_connect::init());
    ESP_ERROR_CHECK(spotify_api::init());
    ESP_ERROR_CHECK(status_led::init());
    ESP_ERROR_CHECK(console::init());

    ESP_LOGI(TAG, "Boot complete");
}

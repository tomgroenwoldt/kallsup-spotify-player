#include "status_led.h"

#include <cmath>

#include "alarm.h"
#include "board_pins.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led_strip.h"
#include "net.h"
#include "settings.h"
#include "spotify_connect.h"

namespace status_led {

static const char *TAG = "status_led";

constexpr TickType_t FRAME = pdMS_TO_TICKS(40);  // 25 fps

static led_strip_handle_t s_strip;

struct Color {
    uint8_t r, g, b;
};

static Color scale(Color c, float k) {
    return {(uint8_t)(c.r * k), (uint8_t)(c.g * k), (uint8_t)(c.b * k)};
}

// Breathing between 10 % and 100 % with the given period.
static float breathe(uint32_t ms, uint32_t period_ms) {
    const float phase = (float)(ms % period_ms) / period_ms;
    return 0.1f + 0.9f * (0.5f - 0.5f * cosf(phase * 6.2831853f));
}

static Color current(uint32_t ms) {
    using alarm_clock::State;
    const State st = alarm_clock::state();
    if (alarm_clock::ringing()) return scale({60, 40, 20}, breathe(ms, 2000));
    if (st == State::SNOOZED) return scale({0, 0, 20}, breathe(ms, 4000));
    if (!net::status().sta_connected || !alarm_clock::time_ok()) {
        return (ms % 1000) < 500 ? Color{24, 0, 0} : Color{0, 0, 0};
    }
    const settings::Config cfg = settings::get();
    if (!cfg.spotify_refresh_token.empty() && !spotify_connect::logged_in()) return {10, 6, 0};
    return {0, 3, 0};
}

static void led_task(void *) {
    Color shown = {1, 1, 1};  // force the first refresh
    for (;;) {
        const uint32_t ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
        const Color c = current(ms);
        if (c.r != shown.r || c.g != shown.g || c.b != shown.b) {
            led_strip_set_pixel(s_strip, 0, c.r, c.g, c.b);
            led_strip_refresh(s_strip);
            shown = c;
        }
        vTaskDelay(FRAME);
    }
}

esp_err_t init() {
    led_strip_config_t strip_cfg = {};
    strip_cfg.strip_gpio_num = pins::STATUS_LED;
    strip_cfg.max_leds = 1;
    strip_cfg.led_model = LED_MODEL_WS2812;
    strip_cfg.color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB;
    led_strip_rmt_config_t rmt_cfg = {};
    rmt_cfg.clk_src = RMT_CLK_SRC_DEFAULT;
    rmt_cfg.resolution_hz = 10 * 1000 * 1000;
    ESP_RETURN_ON_ERROR(led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &s_strip), TAG, "led strip");
    BaseType_t ok = xTaskCreate(led_task, "status_led", 3072, nullptr, 2, nullptr);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "task");
    return ESP_OK;
}

}  // namespace status_led

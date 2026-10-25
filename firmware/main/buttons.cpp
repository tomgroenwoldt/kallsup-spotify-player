#include "buttons.h"

#include "board_pins.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace buttons {

static const char *TAG = "buttons";

constexpr TickType_t POLL = pdMS_TO_TICKS(10);
constexpr int STABLE_POLLS = 3;  // 30 ms debounce
constexpr TickType_t CLICK_GAP = pdMS_TO_TICKS(CLICK_GAP_MS);
constexpr TickType_t HOLD = pdMS_TO_TICKS(HOLD_MS);
constexpr TickType_t REPEAT = pdMS_TO_TICKS(REPEAT_MS);

struct Input {
    gpio_num_t pin;
    Button button;
    const char *name;
    bool pressed = false;
    int stable = 0;
    int clicks = 0;  // in the current series
    TickType_t changed_at = 0;  // last debounced press or release
    TickType_t next_hold = 0;   // when the held press reports next; 0 if not held yet
};

static Input s_inputs[] = {
    {pins::BUTTON_SNOOZE, Button::SNOOZE, "snooze"},
    {pins::BUTTON_STOP, Button::STOP, "stop"},
};
static Handler s_on_event;

static void buttons_task(void *) {
    for (;;) {
        for (Input &in : s_inputs) {
            const TickType_t now = xTaskGetTickCount();
            const bool down = gpio_get_level(in.pin) == 0;
            if (down == in.pressed) {
                in.stable = 0;
                if (down && in.next_hold == 0 && now - in.changed_at >= HOLD) {
                    ESP_LOGI(TAG, "%s x%d held", in.name, in.clicks);
                    s_on_event(in.button, Event::HOLD, in.clicks);
                    in.next_hold = now + REPEAT;
                } else if (down && in.next_hold != 0 && (int32_t)(now - in.next_hold) >= 0) {
                    s_on_event(in.button, Event::REPEAT, in.clicks);
                    in.next_hold += REPEAT;
                } else if (!down && in.clicks > 0 && now - in.changed_at >= CLICK_GAP) {
                    ESP_LOGI(TAG, "%s x%d", in.name, in.clicks);
                    s_on_event(in.button, Event::CLICKS, in.clicks);
                    in.clicks = 0;
                }
                continue;
            }
            if (++in.stable < STABLE_POLLS) continue;
            in.pressed = down;
            in.stable = 0;
            in.changed_at = now;
            if (down) {
                in.clicks++;
            } else if (in.next_hold != 0) {
                in.clicks = 0;  // a hold ends the series
                in.next_hold = 0;
            }
        }
        vTaskDelay(POLL);
    }
}

esp_err_t init(Handler on_event) {
    s_on_event = on_event;
    gpio_config_t cfg = {};
    for (const Input &in : s_inputs) cfg.pin_bit_mask |= 1ULL << in.pin;
    cfg.mode = GPIO_MODE_INPUT;
    cfg.pull_up_en = GPIO_PULLUP_ENABLE;
    ESP_RETURN_ON_ERROR(gpio_config(&cfg), TAG, "gpio");
    BaseType_t ok = xTaskCreate(buttons_task, "buttons", 3072, nullptr, 8, nullptr);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "task");
    return ESP_OK;
}

}  // namespace buttons

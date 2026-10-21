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

struct Input {
    gpio_num_t pin;
    Button button;
    const char *name;
    bool pressed = false;
    int stable = 0;
};

static Input s_inputs[] = {
    {pins::BUTTON_SNOOZE, Button::SNOOZE, "snooze"},
    {pins::BUTTON_STOP, Button::STOP, "stop"},
};
static Handler s_on_press;

static void buttons_task(void *) {
    for (;;) {
        for (Input &in : s_inputs) {
            const bool down = gpio_get_level(in.pin) == 0;
            if (down == in.pressed) {
                in.stable = 0;
                continue;
            }
            if (++in.stable < STABLE_POLLS) continue;
            in.pressed = down;
            in.stable = 0;
            if (down) {
                ESP_LOGI(TAG, "%s pressed", in.name);
                s_on_press(in.button);
            }
        }
        vTaskDelay(POLL);
    }
}

esp_err_t init(Handler on_press) {
    s_on_press = on_press;
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

#pragma once
// GPIO assignments for the ESP32-S3 N16R8 devkit. See docs/WIRING.md.

#include "driver/gpio.h"

namespace pins {

constexpr gpio_num_t I2S_BCLK = GPIO_NUM_5;
constexpr gpio_num_t I2S_WS = GPIO_NUM_6;
constexpr gpio_num_t I2S_DOUT = GPIO_NUM_7;
constexpr gpio_num_t AMP_SD = GPIO_NUM_8;  // MAX98357A SD: LOW = shutdown, HIGH = on

constexpr gpio_num_t BUTTON_SNOOZE = GPIO_NUM_9;
constexpr gpio_num_t BUTTON_STOP = GPIO_NUM_10;

// SSD1306 OLED, I2C. Powered from 3V3, so its pull-ups stay at 3.3 V.
constexpr gpio_num_t OLED_SDA = GPIO_NUM_11;
constexpr gpio_num_t OLED_SCL = GPIO_NUM_12;

// Onboard WS2812 RGB LED. Most N16R8 devkits use GPIO 48, some GPIO 38.
constexpr gpio_num_t STATUS_LED = GPIO_NUM_48;

}  // namespace pins

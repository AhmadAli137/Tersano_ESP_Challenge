#pragma once

#include <cstdint>

#include "driver/gpio.h"
#include "esp_err.h"
#include "led_strip.h"

/*
 * ActuatorHal controls user-facing outputs:
 * - one addressable RGB LED (WS2812/NeoPixel style) on a single GPIO data line
 * - one piezo buzzer driven by LEDC PWM
 */
class ActuatorHal {
 public:
  // pin/channel assignment is injected so board variants can reuse this HAL.
  ActuatorHal(gpio_num_t rgb_data_pin,
              gpio_num_t piezo_pin,
              uint8_t piezo_channel);

  // Initialize LED strip backend and piezo PWM resources.
  esp_err_t begin();

  // Update RGB LED color for pixel index 0.
  esp_err_t setRgb(uint8_t r, uint8_t g, uint8_t b);

  // Enable/disable buzzer; when enabled, apply requested tone frequency.
  esp_err_t setBuzzer(bool on, uint16_t frequency_hz);

 private:
  gpio_num_t rgb_data_pin_;
  gpio_num_t piezo_pin_;
  uint8_t piezo_channel_;
  led_strip_handle_t led_strip_ = nullptr;
  bool initialized_ = false;
};

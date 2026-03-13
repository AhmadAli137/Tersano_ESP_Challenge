#pragma once

#include <cstdint>

#include "driver/gpio.h"
#include "esp_err.h"
#include "led_strip.h"

/*
 * ActuatorHal controls user-facing outputs and exposes a minimal, testable API.
 *
 * Outputs:
 * - one addressable RGB LED (WS2812/NeoPixel style) on a single GPIO data line
 * - one piezo buzzer driven by LEDC PWM
 *
 * Design:
 * - `begin()` is idempotent
 * - write methods return esp_err_t so callers can log/recover without crashing
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
  // Play a short blocking tone pulse, then stop buzzer.
  esp_err_t playTone(uint16_t frequency_hz, uint32_t duration_ms);

 private:
  gpio_num_t rgb_data_pin_;
  gpio_num_t piezo_pin_;
  uint8_t piezo_channel_;
  led_strip_handle_t led_strip_ = nullptr;
  bool initialized_ = false;
};

#pragma once

#include <cstdint>

#include "driver/gpio.h"
#include "esp_err.h"
#include "led_strip.h"

class ActuatorHal {
 public:
  ActuatorHal(gpio_num_t rgb_data_pin,
              gpio_num_t piezo_pin,
              uint8_t piezo_channel);

  esp_err_t begin();
  esp_err_t setRgb(uint8_t r, uint8_t g, uint8_t b);
  esp_err_t setBuzzer(bool on, uint16_t frequency_hz);

 private:
  gpio_num_t rgb_data_pin_;
  gpio_num_t piezo_pin_;
  uint8_t piezo_channel_;
  led_strip_handle_t led_strip_ = nullptr;
  bool initialized_ = false;
};

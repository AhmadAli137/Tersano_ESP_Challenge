#include "actuator_hal.h"

#include "driver/ledc.h"
#include "esp_check.h"

#include "app_config.h"

// Constructor stores board mapping; hardware allocation happens in begin().
ActuatorHal::ActuatorHal(gpio_num_t rgb_data_pin,
                         gpio_num_t piezo_pin,
                         uint8_t piezo_channel)
    : rgb_data_pin_(rgb_data_pin),
      piezo_pin_(piezo_pin),
      piezo_channel_(piezo_channel) {}

esp_err_t ActuatorHal::begin() {
  // Safe to call multiple times (for example after subsystem restart).
  if (initialized_) {
    return ESP_OK;
  }

  // Configure single-pixel LED strip transport (RMT backend).
  led_strip_config_t strip_config = {};
  strip_config.strip_gpio_num = rgb_data_pin_;
  strip_config.max_leds = 1;

  led_strip_rmt_config_t rmt_config = {};
  rmt_config.resolution_hz = 10 * 1000 * 1000;
  rmt_config.flags.with_dma = false;
  ESP_RETURN_ON_ERROR(led_strip_new_rmt_device(&strip_config, &rmt_config, &led_strip_), "ActuatorHal", "RGB init failed");
  ESP_RETURN_ON_ERROR(led_strip_clear(led_strip_), "ActuatorHal", "RGB clear failed");

  // Piezo uses a dedicated low-speed timer/channel so tone frequency can be changed at runtime.
  ledc_timer_config_t piezo_timer = {};
  piezo_timer.speed_mode = LEDC_LOW_SPEED_MODE;
  piezo_timer.timer_num = LEDC_TIMER_1;
  piezo_timer.duty_resolution = LEDC_TIMER_10_BIT;
  piezo_timer.freq_hz = appcfg::PIEZO_DEFAULT_FREQ_HZ;
  piezo_timer.clk_cfg = LEDC_AUTO_CLK;
  ESP_RETURN_ON_ERROR(ledc_timer_config(&piezo_timer), "ActuatorHal", "Piezo timer init failed");

  ledc_channel_config_t ch = {};
  ch.speed_mode = LEDC_LOW_SPEED_MODE;
  ch.timer_sel = LEDC_TIMER_1;
  ch.channel = static_cast<ledc_channel_t>(piezo_channel_);
  ch.gpio_num = piezo_pin_;
  ch.intr_type = LEDC_INTR_DISABLE;
  ch.duty = 0;
  ch.hpoint = 0;
  ESP_RETURN_ON_ERROR(ledc_channel_config(&ch), "ActuatorHal", "Piezo channel init failed");

  initialized_ = true;
  return ESP_OK;
}

esp_err_t ActuatorHal::setRgb(uint8_t r, uint8_t g, uint8_t b) {
  if (!initialized_ || led_strip_ == nullptr) {
    return ESP_ERR_INVALID_STATE;
  }
  // Keep RGB write atomic at HAL level: set pixel then refresh immediately.
  ESP_RETURN_ON_ERROR(led_strip_set_pixel(led_strip_, 0, r, g, b), "ActuatorHal", "RGB set pixel failed");
  return led_strip_refresh(led_strip_);
}

esp_err_t ActuatorHal::setBuzzer(bool on, uint16_t frequency_hz) {
  if (!initialized_) {
    return ESP_ERR_INVALID_STATE;
  }

  if (!on) {
    ESP_RETURN_ON_ERROR(ledc_set_duty(LEDC_LOW_SPEED_MODE, static_cast<ledc_channel_t>(piezo_channel_), 0), "ActuatorHal", "Piezo duty off failed");
    return ledc_update_duty(LEDC_LOW_SPEED_MODE, static_cast<ledc_channel_t>(piezo_channel_));
  }

  // ~50% duty provides audible tone while limiting drive stress.
  ESP_RETURN_ON_ERROR(ledc_set_freq(LEDC_LOW_SPEED_MODE, LEDC_TIMER_1, frequency_hz), "ActuatorHal", "Piezo freq set failed");
  ESP_RETURN_ON_ERROR(ledc_set_duty(LEDC_LOW_SPEED_MODE, static_cast<ledc_channel_t>(piezo_channel_), 512), "ActuatorHal", "Piezo duty on failed");
  return ledc_update_duty(LEDC_LOW_SPEED_MODE, static_cast<ledc_channel_t>(piezo_channel_));
}

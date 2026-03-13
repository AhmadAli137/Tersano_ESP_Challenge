#pragma once

// Centralized compile-time configuration for board pins and app behavior.
// Updating this file is the safest way to retarget hardware mappings.

#include <cstddef>

#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"

#if __has_include("secrets.h")
#include "secrets.h"
#else
#include "secrets.example.h"
#endif

namespace appcfg {

// Stable logical identifier for cloud/backend routing.
static constexpr const char* DEVICE_ID = "rtu-esp32c5-01";

// Digital IO pin mappings.
static constexpr gpio_num_t STATUS_LED_PIN = GPIO_NUM_2;
static constexpr gpio_num_t I2C_SDA_PIN = GPIO_NUM_4;
static constexpr gpio_num_t I2C_SCL_PIN = GPIO_NUM_5;
static constexpr gpio_num_t BATTERY_ADC_PIN = GPIO_NUM_0;
static constexpr adc_channel_t BATTERY_ADC_CHANNEL = ADC_CHANNEL_0;

// RGB + piezo outputs.
static constexpr gpio_num_t RGB_PIN_R = GPIO_NUM_18;
static constexpr gpio_num_t RGB_PIN_G = GPIO_NUM_19;
static constexpr gpio_num_t RGB_PIN_B = GPIO_NUM_20;
static constexpr gpio_num_t PIEZO_PIN = GPIO_NUM_10;

// LEDC channel/frequency setup for PWM peripherals.
static constexpr uint8_t LEDC_CHANNEL_R = 0;
static constexpr uint8_t LEDC_CHANNEL_G = 1;
static constexpr uint8_t LEDC_CHANNEL_B = 2;
static constexpr uint8_t LEDC_CHANNEL_PIEZO = 3;
static constexpr uint32_t LEDC_FREQ_RGB_HZ = 5000;
static constexpr uint8_t LEDC_BITS_RGB = 8;
static constexpr uint32_t PIEZO_DEFAULT_FREQ_HZ = 2000;

// ADC conversion constants for battery estimation.
static constexpr float ADC_REF_VOLTAGE = 3.3f;
static constexpr float BATTERY_DIVIDER_RATIO = 2.0f;  // Example 100k/100k divider.
static constexpr uint16_t ADC_MAX = 4095;

// Runtime scheduling defaults and safety bounds.
static constexpr uint32_t DEFAULT_SAMPLING_INTERVAL_MS = 5000;
static constexpr uint32_t MIN_SAMPLING_INTERVAL_MS = 1000;
static constexpr uint32_t MAX_SAMPLING_INTERVAL_MS = 600000;
static constexpr uint32_t COMMAND_POLL_INTERVAL_MS = 1000;

// Local persistence paths/limits.
static constexpr const char* BACKLOG_FILE = "/backlog.ndjson";
static constexpr size_t MAX_BACKLOG_LINES = 2000;

// Backend table names.
static constexpr const char* TABLE_TELEMETRY = "telemetry";
static constexpr const char* TABLE_COMMANDS = "device_commands";
static constexpr const char* TABLE_STATUS = "device_status";

}  // namespace appcfg

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

// Actuator outputs.
// RGB is a single addressable LED (WS2812/NeoPixel style) on one data pin.
static constexpr gpio_num_t RGB_DATA_PIN = GPIO_NUM_27;
static constexpr gpio_num_t PIEZO_PIN = GPIO_NUM_10;
// RGB channel remap for boards/LEDs with non-standard color ordering.
// Each value is an index into input channels: 0=R, 1=G, 2=B.
// Example for GRB strips: R_INDEX=1, G_INDEX=0, B_INDEX=2.
static constexpr uint8_t RGB_ORDER_R_INDEX = 1;
static constexpr uint8_t RGB_ORDER_G_INDEX = 0;
static constexpr uint8_t RGB_ORDER_B_INDEX = 2;

// LEDC channel/frequency setup for piezo PWM output.
static constexpr uint8_t LEDC_CHANNEL_PIEZO = 0;
static constexpr uint32_t PIEZO_DEFAULT_FREQ_HZ = 2000;

// ADC conversion constants for battery estimation.
static constexpr float ADC_REF_VOLTAGE = 3.3f;
static constexpr float BATTERY_DIVIDER_RATIO = 2.0f;  // Example 100k/100k divider.
static constexpr uint16_t ADC_MAX = 4095;

// Runtime scheduling defaults and safety bounds.
static constexpr uint32_t DEFAULT_SAMPLING_INTERVAL_MS = 5000;
static constexpr uint32_t MIN_SAMPLING_INTERVAL_MS = 1000;
static constexpr uint32_t MAX_SAMPLING_INTERVAL_MS = 600000;
static constexpr uint32_t COMMAND_POLL_INTERVAL_MS = 5000;

// Local persistence paths/limits.
static constexpr const char* BACKLOG_FILE = "/backlog.ndjson";
static constexpr size_t MAX_BACKLOG_LINES = 2000;

// Backend table names.
static constexpr const char* TABLE_TELEMETRY = "telemetry";
static constexpr const char* TABLE_COMMANDS = "commands";
static constexpr const char* TABLE_STATUS = "status";

}  // namespace appcfg

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

// Digital IO pin mappings.
// Note: I2C is mapped to GPIO2/GPIO3 to match board silk/LP_I2C labels.
// STATUS_LED_PIN is moved off GPIO2 to avoid collision with I2C SDA.
static constexpr gpio_num_t STATUS_LED_PIN = GPIO_NUM_24;
static constexpr gpio_num_t I2C_SDA_PIN = GPIO_NUM_2;
static constexpr gpio_num_t I2C_SCL_PIN = GPIO_NUM_3;
static constexpr gpio_num_t BATTERY_ADC_PIN = GPIO_NUM_6;
static constexpr adc_channel_t BATTERY_ADC_CHANNEL = ADC_CHANNEL_5;

// Actuator outputs.
// RGB is a single addressable LED (WS2812/NeoPixel style) on one data pin.
static constexpr gpio_num_t RGB_DATA_PIN = GPIO_NUM_27;
static constexpr gpio_num_t PIEZO_PIN = GPIO_NUM_26;
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
static constexpr float BATTERY_DIVIDER_RATIO = 2.06f;  // Example 10k/10k divider.
static constexpr uint16_t ADC_MAX = 4095;

// Runtime scheduling defaults and safety bounds.
static constexpr uint32_t DEFAULT_SAMPLING_INTERVAL_MS = 300000;   // 5 minutes (Default)
static constexpr uint32_t MIN_SAMPLING_INTERVAL_MS = 5000;         // 5 seconds (Fast preset)
static constexpr uint32_t MAX_SAMPLING_INTERVAL_MS = 1800000;      // 30 minutes (Slow preset)
// Base command poll cadence. NetworkHal may apply adaptive backoff when idle.
static constexpr uint32_t COMMAND_POLL_INTERVAL_MS = 1000;

// Local persistence paths/limits.
static constexpr const char* BACKLOG_FILE = "/backlog.ndjson";
static constexpr size_t MAX_BACKLOG_LINES = 2000;
// If backlog grows too large, trim to most recent lines on startup.
static constexpr size_t BACKLOG_STARTUP_TRIM_THRESHOLD = 500;
static constexpr size_t BACKLOG_STARTUP_KEEP_LINES = 200;
// Throttle replay so live telemetry remains responsive under heavy backlog.
static constexpr uint32_t BACKLOG_FLUSH_INTERVAL_MS = 1200;

// Backend table names.
static constexpr const char* TABLE_TELEMETRY = "telemetry";
static constexpr const char* TABLE_COMMANDS = "device_commands";
static constexpr const char* TABLE_STATUS = "status";

}  // namespace appcfg

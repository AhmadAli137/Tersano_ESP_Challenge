/*
 * Application entry point.
 * This file intentionally stays small and focused on orchestration:
 * - initialize subsystems
 * - run the main loop/timing
 * - delegate hardware details to modules (for example, actuator_hal)
 */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "sdkconfig.h"

#include "actuator_hal.h"
#include "app_config.h"

// Logging tag used by ESP-IDF log output.
static const char *TAG = "main";

// Keep a sane default period if Kconfig is not enabled yet.
#ifndef CONFIG_BLINK_PERIOD
#define CONFIG_BLINK_PERIOD 1000
#endif

extern "C" void app_main(void)
{
    // Actuator subsystem owns both RGB status LED and piezo tone output.
    ActuatorHal actuator(appcfg::RGB_DATA_PIN, appcfg::PIEZO_PIN, appcfg::LEDC_CHANNEL_PIEZO);
    ESP_ERROR_CHECK(actuator.begin());

    bool led_on = false;

    // Main heartbeat loop:
    // - blink RGB pixel to indicate scheduler alive
    // - keep buzzer off in idle state
    while (1) {
        if (led_on) {
            ESP_ERROR_CHECK(actuator.setRgb(16, 16, 16));
        } else {
            ESP_ERROR_CHECK(actuator.setRgb(0, 0, 0));
        }
        ESP_ERROR_CHECK(actuator.setBuzzer(false, appcfg::PIEZO_DEFAULT_FREQ_HZ));
        ESP_LOGI(TAG, "RGB LED is now %s", led_on ? "ON" : "OFF");
        led_on = !led_on;
        vTaskDelay(CONFIG_BLINK_PERIOD / portTICK_PERIOD_MS);
    }
}

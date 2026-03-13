/*
 * Application entry point.
 * This file intentionally stays small and focused on orchestration:
 * - initialize subsystems
 * - run the main loop/timing
 * - delegate hardware details to modules (for example, led_controller)
 */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "sdkconfig.h"

#include "led_controller.h"

// Logging tag used by ESP-IDF log output.
static const char *TAG = "main";

// Keep a sane default period if Kconfig is not enabled yet.
#ifndef CONFIG_BLINK_PERIOD
#define CONFIG_BLINK_PERIOD 1000
#endif

extern "C" void app_main(void)
{
    // Initialize LED hardware/backend once at startup.
    ESP_ERROR_CHECK(led_controller::init());

    // Main heartbeat loop: toggle LED and wait for the configured interval.
    while (1) {
        ESP_ERROR_CHECK(led_controller::toggle());
        ESP_LOGI(TAG, "LED is now %s", led_controller::is_on() ? "ON" : "OFF");
        vTaskDelay(CONFIG_BLINK_PERIOD / portTICK_PERIOD_MS);
    }
}

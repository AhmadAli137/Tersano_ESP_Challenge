/*
 * RTU firmware entrypoint.
 *
 * Responsibilities in this file:
 * 1) Configure human-friendly log levels for operator use
 * 2) Bootstrap the top-level controller with bounded retries
 * 3) Keep app_main minimal and delegate runtime work to controller tasks
 *
 * Hardware/protocol specifics intentionally live in HAL/controller modules.
 */

#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "rtu_controller.h"

static const char *TAG = "main";
// Retry interval used if RTU initialization fails due to transient subsystem issues.
static constexpr uint32_t kInitRetryDelayMs = 2000;
// Safety cap: reboot if init keeps failing so device self-recovers in the field.
static constexpr uint32_t kMaxInitAttemptsBeforeRestart = 5;

static void configureOperatorLogs() {
    // Keep application logs informative while reducing low-level driver noise.
    esp_log_level_set("*", ESP_LOG_INFO);
    esp_log_level_set("main", ESP_LOG_INFO);
    esp_log_level_set("RtuController", ESP_LOG_INFO);
    esp_log_level_set("NetworkHal", ESP_LOG_INFO);
    esp_log_level_set("SensorHal", ESP_LOG_INFO);
    esp_log_level_set("BacklogStore", ESP_LOG_INFO);
    esp_log_level_set("ActuatorHal", ESP_LOG_INFO);

    // These tags are useful for debugging, but too verbose for day-to-day monitoring.
    // Keep low-level radio/PHY chatter out of normal operator logs.
    esp_log_level_set("wifi", ESP_LOG_ERROR);
    esp_log_level_set("wifi_init", ESP_LOG_ERROR);
    esp_log_level_set("pp", ESP_LOG_ERROR);
    esp_log_level_set("net80211", ESP_LOG_ERROR);
    esp_log_level_set("phy_init", ESP_LOG_ERROR);
    esp_log_level_set("esp-x509-crt-bundle", ESP_LOG_ERROR);
    esp_log_level_set("i2c", ESP_LOG_WARN);
}

extern "C" void app_main(void)
{
    configureOperatorLogs();

    ESP_LOGI(TAG, "Starting RTU controller bootstrap...");
    ESP_LOGI(TAG, "[Stage 1/3] Initializing subsystems (sensor, network, actuator, backlog)");

    static RtuController rtu;

    uint32_t init_attempt = 0;
    while (!rtu.isStarted()) {
        ++init_attempt;
        ESP_LOGI(TAG, "RTU init attempt %lu", static_cast<unsigned long>(init_attempt));
        rtu.begin();
        if (rtu.isStarted()) {
            ESP_LOGI(TAG, "RTU init successful");
            break;
        }

        ESP_LOGW(TAG, "RTU init failed, retrying in %lu ms", static_cast<unsigned long>(kInitRetryDelayMs));
        if (init_attempt >= kMaxInitAttemptsBeforeRestart) {
            ESP_LOGE(TAG, "RTU failed to initialize after %lu attempts, restarting",
                     static_cast<unsigned long>(kMaxInitAttemptsBeforeRestart));
            vTaskDelay(pdMS_TO_TICKS(300));
            esp_restart();
        }
        vTaskDelay(pdMS_TO_TICKS(kInitRetryDelayMs));
    }

    ESP_LOGI(TAG, "[Stage 2/3] Background tasks are running");
    ESP_LOGI(TAG, "[Stage 3/3] Entering steady-state loop");

    // Keep app_main lightweight: controller tasks do the actual work.
    // rtu.loop() owns its pacing, so we avoid adding an extra delay here.
    while (true) {
        rtu.loop();
    }
}

#include "rtu_controller.h"

#include <algorithm>
#include <cinttypes>
#include <cstdio>

#include "cJSON.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"

namespace {
constexpr const char* kTag = "RtuController";
// If this many publish attempts fail in a row, LED shows cloud-degraded state (yellow).
constexpr uint32_t kCloudFailStreakThreshold = 3;
// If no successful publish happened for this long, cloud is considered stale/degraded.
constexpr uint32_t kCloudStaleThresholdMs = 15000;
}

RtuController::RtuController()
    : sensor_(static_cast<uint8_t>(appcfg::I2C_SDA_PIN),
              static_cast<uint8_t>(appcfg::I2C_SCL_PIN),
              static_cast<uint8_t>(appcfg::BATTERY_ADC_PIN)),
      actuator_(appcfg::RGB_DATA_PIN,
                appcfg::PIEZO_PIN,
                appcfg::LEDC_CHANNEL_PIEZO),
      net_(secrets::WIFI_SSID,
           secrets::WIFI_PASS,
           secrets::SUPABASE_URL,
           secrets::SUPABASE_API_KEY,
           appcfg::TABLE_TELEMETRY,
           appcfg::TABLE_COMMANDS,
           appcfg::TABLE_STATUS,
           appcfg::DEVICE_ID) {}

void RtuController::begin() {
  // begin() can be called multiple times by bootstrap logic; initialize exactly once.
  if (started_) {
    ESP_LOGW(kTag, "begin() called more than once, ignoring");
    return;
  }

  gpio_config_t io = {};
  io.mode = GPIO_MODE_OUTPUT;
  io.pin_bit_mask = 1ULL << static_cast<uint8_t>(appcfg::STATUS_LED_PIN);
  gpio_config(&io);
  gpio_set_level(appcfg::STATUS_LED_PIN, 0);

  ESP_LOGI(kTag, "Booting RTU...");
  ESP_LOGI(kTag, "Initializing actuator, sensor, network, and backlog subsystems");

  if (actuator_.begin() != ESP_OK) {
    ESP_LOGE(kTag, "Actuator init failed");
  } else {
    ESP_LOGI(kTag, "Actuator init OK");
  }
  sensor_.begin();
  net_.begin();
  net_.setCommandHandler([this](const std::string& body) { onCommand(body); });

  if (!backlog_.begin(appcfg::BACKLOG_FILE, appcfg::MAX_BACKLOG_LINES)) {
    ESP_LOGE(kTag, "SPIFFS/backlog init failed");
  } else {
    ESP_LOGI(kTag, "Backlog store mounted and ready");
  }

  // Queue decouples sampling cadence from network variability.
  sample_queue_ = xQueueCreate(32, sizeof(TelemetrySample));
  if (!sample_queue_) {
    ESP_LOGE(kTag, "Failed to create sample queue");
    return;
  }

  cfg_lock_ = xSemaphoreCreateMutex();
  if (!cfg_lock_) {
    ESP_LOGE(kTag, "Failed to create config mutex");
    vQueueDelete(sample_queue_);
    sample_queue_ = nullptr;
    return;
  }

  updateStatusRgb();

  // Best-effort startup heartbeat to backend. Failure is non-fatal.
  net_.publishStatus(statusToJson("boot"));

  TaskHandle_t sample_task = nullptr;
  TaskHandle_t publish_task = nullptr;
  TaskHandle_t conn_task = nullptr;
  const BaseType_t sample_task_ok = xTaskCreate(sampleTaskThunk, "sampleTask", 4096, this, 5, &sample_task);
  const BaseType_t publish_task_ok = xTaskCreate(publishTaskThunk, "publishTask", 6144, this, 5, &publish_task);
  const BaseType_t conn_task_ok = xTaskCreate(connectivityTaskThunk, "connTask", 4096, this, 5, &conn_task);
  if (sample_task_ok != pdPASS || publish_task_ok != pdPASS || conn_task_ok != pdPASS) {
    ESP_LOGE(kTag, "Failed to create one or more controller tasks");
    if (sample_task) vTaskDelete(sample_task);
    if (publish_task) vTaskDelete(publish_task);
    if (conn_task) vTaskDelete(conn_task);
    if (cfg_lock_) vSemaphoreDelete(cfg_lock_);
    if (sample_queue_) vQueueDelete(sample_queue_);
    cfg_lock_ = nullptr;
    sample_queue_ = nullptr;
    return;
  }

  started_ = true;
  ESP_LOGI(kTag, "RTU controller started (sampling, publish, and connectivity tasks active)");
}

void RtuController::loop() {
  // app_main loop stays intentionally thin; all real work lives in tasks.
  if (!started_) {
    vTaskDelay(pdMS_TO_TICKS(250));
    return;
  }
  vTaskDelay(pdMS_TO_TICKS(100));
}

void RtuController::sampleTaskThunk(void* ctx) {
  static_cast<RtuController*>(ctx)->sampleTask();
}

void RtuController::publishTaskThunk(void* ctx) {
  static_cast<RtuController*>(ctx)->publishTask();
}

void RtuController::connectivityTaskThunk(void* ctx) {
  static_cast<RtuController*>(ctx)->connectivityTask();
}

void RtuController::sampleTask() {
  while (true) {
    // Sequence increments here so every produced sample gets a unique order id.
    const TelemetrySample sample = sensor_.read(++sequence_);
    if (xQueueSend(sample_queue_, &sample, pdMS_TO_TICKS(50)) != pdPASS) {
      ESP_LOGW(kTag, "Sample queue full, dropping");
    } else {
      ESP_LOGI(kTag,
               "S#%" PRIu32 " T=%.2fC H=%.2f%% P=%.2fhPa Vbat=%.2fV sensor_ok=%d",
               sample.seq,
               sample.temperature_c,
               sample.humidity_pct,
               sample.pressure_hpa,
               sample.battery_v,
               sample.sensor_ok);
    }

    uint32_t interval = appcfg::DEFAULT_SAMPLING_INTERVAL_MS;
    if (cfg_lock_ && xSemaphoreTake(cfg_lock_, pdMS_TO_TICKS(20)) == pdTRUE) {
      interval = sample_interval_ms_;
      xSemaphoreGive(cfg_lock_);
    }
    vTaskDelay(pdMS_TO_TICKS(interval));
  }
}

void RtuController::publishTask() {
  TelemetrySample sample = {};
  while (true) {
    // Block until there is sampled data to publish.
    if (xQueueReceive(sample_queue_, &sample, pdMS_TO_TICKS(200)) != pdTRUE) continue;
    const std::string payload = sampleToJson(sample);

    if (net_.publishTelemetry(payload)) {
      markPublishResult(true);
      ESP_LOGI(kTag, "Telemetry sent");
      continue;
    }
    markPublishResult(false);

    if (backlog_.appendLine(payload)) {
      ESP_LOGW(kTag, "Offline, cached (%u lines)", static_cast<unsigned>(backlog_.countLines()));
    } else {
      ESP_LOGE(kTag, "Failed to cache sample");
    }
  }
}

void RtuController::connectivityTask() {
  while (true) {
    // Drive network polling state machine (commands, connectivity housekeeping).
    net_.loop();
    updateStatusRgb();

    // Drain one backlog row per iteration to avoid monopolizing network time.
    if (net_.isConnected() && backlog_.countLines() > 0) {
      std::string row;
      if (backlog_.popOldestLine(row)) {
        if (net_.publishTelemetry(row)) {
          markPublishResult(true);
          ESP_LOGI(kTag, "Flushed backlog (%u left)", static_cast<unsigned>(backlog_.countLines()));
        } else {
          markPublishResult(false);
          backlog_.prependLine(row);
          vTaskDelay(pdMS_TO_TICKS(500));
        }
      }
    }
    vTaskDelay(pdMS_TO_TICKS(150));
  }
}

void RtuController::onCommand(const std::string& json) {
  // Commands are expected as JSON object, for example:
  // {"sampling_interval_ms":10000,"buzzer":{"on":true,"frequency_hz":1200}}
  cJSON* doc = cJSON_Parse(json.c_str());
  if (!doc) {
    ESP_LOGW(kTag, "Invalid command JSON");
    return;
  }

  bool changed_any = false;

  cJSON* sampling = cJSON_GetObjectItem(doc, "sampling_interval_ms");
  if (cJSON_IsNumber(sampling) && sampling->valueint > 0) {
    const uint32_t bounded = static_cast<uint32_t>(
        std::clamp(sampling->valueint,
                   static_cast<int>(appcfg::MIN_SAMPLING_INTERVAL_MS),
                   static_cast<int>(appcfg::MAX_SAMPLING_INTERVAL_MS)));
    if (cfg_lock_ && xSemaphoreTake(cfg_lock_, pdMS_TO_TICKS(50)) == pdTRUE) {
      sample_interval_ms_ = bounded;
      xSemaphoreGive(cfg_lock_);
    }
    changed_any = true;
    ESP_LOGI(kTag, "Command sampling_interval_ms=%" PRIu32, bounded);
  }

  cJSON* buzzer = cJSON_GetObjectItem(doc, "buzzer");
  if (buzzer && cJSON_IsObject(buzzer)) {
    cJSON* on = cJSON_GetObjectItem(buzzer, "on");
    cJSON* freq = cJSON_GetObjectItem(buzzer, "frequency_hz");
    const bool buzzer_on = cJSON_IsBool(on) ? cJSON_IsTrue(on) : false;
    const int f = cJSON_IsNumber(freq) ? freq->valueint : static_cast<int>(appcfg::PIEZO_DEFAULT_FREQ_HZ);
    const uint16_t bounded_f = static_cast<uint16_t>(std::clamp(f, 200, 5000));
    if (actuator_.setBuzzer(buzzer_on, bounded_f) != ESP_OK) {
      ESP_LOGW(kTag, "Buzzer command failed");
    }
    changed_any = true;
    ESP_LOGI(kTag, "Command buzzer=%d freq=%u", buzzer_on ? 1 : 0, bounded_f);
  }

  cJSON_Delete(doc);
  // Report command application result to backend for observability.
  if (changed_any) net_.publishStatus(statusToJson("command_applied"));
}

std::string RtuController::sampleToJson(const TelemetrySample& sample) const {
  char out[384];
  std::snprintf(out,
                sizeof(out),
                "{\"device_id\":\"%s\",\"seq\":%" PRIu32 ",\"uptime_ms\":%" PRIu64
                ",\"temperature_c\":%.2f,\"humidity_pct\":%.2f,\"pressure_hpa\":%.2f,"
                "\"battery_v\":%.2f,\"sensor_ok\":%s}",
                appcfg::DEVICE_ID,
                sample.seq,
                sample.uptime_ms,
                sample.temperature_c,
                sample.humidity_pct,
                sample.pressure_hpa,
                sample.battery_v,
                sample.sensor_ok ? "true" : "false");
  return std::string(out);
}

std::string RtuController::statusToJson(const char* event) const {
  char out[192];
  const uint64_t uptime_ms = static_cast<uint64_t>(esp_timer_get_time() / 1000ULL);
  std::snprintf(out,
                sizeof(out),
                "{\"device_id\":\"%s\",\"event\":\"%s\",\"uptime_ms\":%" PRIu64 "}",
                appcfg::DEVICE_ID,
                event,
                uptime_ms);
  return std::string(out);
}

void RtuController::updateStatusRgb() {
  // Red = station disconnected from Wi-Fi.
  if (!net_.isConnected()) {
    if (actuator_.setRgb(255, 0, 0) != ESP_OK) {
      ESP_LOGW(kTag, "Status RGB update failed");
    }
    return;
  }

  const uint32_t now_ms = static_cast<uint32_t>(esp_timer_get_time() / 1000ULL);
  if (isCloudDegraded(now_ms)) {
    // Yellow = Wi-Fi is up but cloud publish path is failing/degraded.
    if (actuator_.setRgb(255, 180, 0) != ESP_OK) {
      ESP_LOGW(kTag, "Status RGB update failed");
    }
    return;
  }

  uint32_t interval = appcfg::DEFAULT_SAMPLING_INTERVAL_MS;
  if (cfg_lock_ && xSemaphoreTake(cfg_lock_, pdMS_TO_TICKS(10)) == pdTRUE) {
    interval = sample_interval_ms_;
    xSemaphoreGive(cfg_lock_);
  }

  if (interval > appcfg::DEFAULT_SAMPLING_INTERVAL_MS) {
    // Blue = intentionally slowed sampling interval.
    if (actuator_.setRgb(0, 0, 255) != ESP_OK) {
      ESP_LOGW(kTag, "Status RGB update failed");
    }
  } else {
    // Green = normal connected/healthy cadence.
    if (actuator_.setRgb(0, 255, 0) != ESP_OK) {
      ESP_LOGW(kTag, "Status RGB update failed");
    }
  }
}

void RtuController::markPublishResult(bool ok) {
  if (ok) {
    has_publish_success_ = true;
    publish_fail_streak_ = 0;
    last_publish_ok_ms_ = static_cast<uint32_t>(esp_timer_get_time() / 1000ULL);
    return;
  }

  if (publish_fail_streak_ < 1000000U) {
    ++publish_fail_streak_;
  }
}

bool RtuController::isCloudDegraded(uint32_t now_ms) const {
  // Degraded if repeated failures are happening now.
  if (publish_fail_streak_ >= kCloudFailStreakThreshold) {
    return true;
  }
  // Degraded if we have historical success but no recent success.
  if (has_publish_success_) {
    return (now_ms - last_publish_ok_ms_) > kCloudStaleThresholdMs;
  }
  // During cold boot, do not mark degraded until first success/fail streak evidence exists.
  return false;
}

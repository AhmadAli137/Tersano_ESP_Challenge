#include "rtu_controller.h"

#include <algorithm>
#include <cinttypes>
#include <cstring>
#include <cstdio>

#include "cJSON.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"

namespace {
constexpr const char* kTag = "RtuController";
// If this many publish attempts fail in a row, LED shows cloud-degraded state (yellow).
constexpr uint32_t kCloudFailStreakThreshold = 3;
// Minimum stale timeout baseline; effective timeout scales with sampling interval.
constexpr uint32_t kCloudStaleThresholdMinMs = 15000;
// Emit lightweight periodic status heartbeat every 30s.
constexpr uint32_t kHeartbeatIntervalMs = 30000;
// Emit data_sync summary every 30s or after this many flushed rows.
constexpr uint32_t kSyncSummaryIntervalMs = 30000;
constexpr uint32_t kSyncSummaryMaxRecords = 25;
constexpr uint32_t kTaskAliveIntervalMs = 30000;
// Command behavior defaults.
constexpr uint16_t kToneDefaultFreqHz = 1200;
constexpr uint32_t kToneDefaultDurationMs = 180;
constexpr uint32_t kLedBlinkPeriodMs = 1000;
constexpr uint32_t kLedBlinkOnWindowMs = 500;
// Sampling-rate presets used by LED color policy.
constexpr uint32_t kSamplingFastMs = 5000;       // 5 seconds
constexpr uint32_t kSamplingDefaultMs = 300000;  // 5 minutes
constexpr uint32_t kSamplingSlowMs = 1800000;    // 30 minutes

bool readWifiLinkMeta(char* ip_out, size_t ip_out_len, int* rssi_out) {
  if (!ip_out || ip_out_len == 0 || !rssi_out) return false;
  ip_out[0] = '\0';
  *rssi_out = 0;

  esp_netif_t* sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
  if (!sta) return false;

  esp_netif_ip_info_t ip_info = {};
  if (esp_netif_get_ip_info(sta, &ip_info) != ESP_OK) return false;
  std::snprintf(ip_out, ip_out_len, IPSTR, IP2STR(&ip_info.ip));

  wifi_ap_record_t ap_info = {};
  if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
    *rssi_out = static_cast<int>(ap_info.rssi);
  }
  return true;
}
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
    if (backlog_.countLines() > appcfg::BACKLOG_STARTUP_TRIM_THRESHOLD) {
      const size_t before = backlog_.countLines();
      if (backlog_.trimToNewest(appcfg::BACKLOG_STARTUP_KEEP_LINES)) {
        ESP_LOGW(kTag,
                 "Backlog trimmed at boot: %u -> %u lines",
                 static_cast<unsigned>(before),
                 static_cast<unsigned>(backlog_.countLines()));
      } else {
        ESP_LOGW(kTag, "Backlog trim failed; continuing with %u lines",
                 static_cast<unsigned>(backlog_.countLines()));
      }
    }
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
#ifndef PROJECT_VER
#define PROJECT_VER "unknown"
#endif
  char boot_meta[192];
  std::snprintf(boot_meta,
                sizeof(boot_meta),
                "{\"firmware\":\"%s\",\"reason\":\"power_on\"}",
                PROJECT_VER);
  publishStatusEvent("boot", boot_meta);

  TaskHandle_t sample_task = nullptr;
  TaskHandle_t publish_task = nullptr;
  TaskHandle_t conn_task = nullptr;
  TaskHandle_t cmd_task = nullptr;
  // Keep healthy stack headroom for JSON/log/network-heavy code paths.
  const BaseType_t sample_task_ok = xTaskCreate(sampleTaskThunk, "sampleTask", 6144, this, 5, &sample_task);
  const BaseType_t publish_task_ok = xTaskCreate(publishTaskThunk, "publishTask", 8192, this, 5, &publish_task);
  const BaseType_t conn_task_ok = xTaskCreate(connectivityTaskThunk, "connTask", 8192, this, 5, &conn_task);
  const BaseType_t cmd_task_ok = xTaskCreate(commandTaskThunk, "commandTask", 6144, this, 5, &cmd_task);
  if (sample_task_ok != pdPASS || publish_task_ok != pdPASS || conn_task_ok != pdPASS || cmd_task_ok != pdPASS) {
    ESP_LOGE(kTag, "Failed to create one or more controller tasks");
    if (sample_task) vTaskDelete(sample_task);
    if (publish_task) vTaskDelete(publish_task);
    if (conn_task) vTaskDelete(conn_task);
    if (cmd_task) vTaskDelete(cmd_task);
    if (cfg_lock_) vSemaphoreDelete(cfg_lock_);
    if (sample_queue_) vQueueDelete(sample_queue_);
    cfg_lock_ = nullptr;
    sample_queue_ = nullptr;
    return;
  }
  sample_task_handle_ = sample_task;

  started_ = true;
  ESP_LOGI(kTag, "RTU controller started (sampling, publish, connectivity, and command tasks active)");
  publishStatusEvent("rtu_started");
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

void RtuController::commandTaskThunk(void* ctx) {
  static_cast<RtuController*>(ctx)->commandTask();
}

void RtuController::sampleTask() {
  uint32_t last_alive_log_ms = 0;
  while (true) {
    // Sequence increments here so every produced sample gets a unique order id.
    const TelemetrySample sample = sensor_.read(++sequence_);
    if (xQueueSend(sample_queue_, &sample, pdMS_TO_TICKS(50)) != pdPASS) {
      ESP_LOGW(kTag, "Sample queue full, dropping");
    } else {
      ESP_LOGI(kTag,
               "Sample #%" PRIu32 ": T=%.2fC H=%.2f%% P=%.2fhPa Vbat=%.2fV sensor_ok=%d",
               sample.seq,
               sample.temperature_c,
               sample.humidity_pct,
               sample.pressure_hpa,
               sample.battery_v,
               sample.sensor_ok);
      if (sample.sensor_ok && !calibration_reported_) {
        publishStatusEvent("calibrated", "{\"sensors\":[\"temp\",\"humidity\",\"pressure\"]}");
        calibration_reported_ = true;
      }
    }

    uint32_t interval = appcfg::DEFAULT_SAMPLING_INTERVAL_MS;
    if (cfg_lock_ && xSemaphoreTake(cfg_lock_, pdMS_TO_TICKS(20)) == pdTRUE) {
      interval = sample_interval_ms_;
      xSemaphoreGive(cfg_lock_);
    }

    const uint32_t now_ms = static_cast<uint32_t>(esp_timer_get_time() / 1000ULL);
    if ((now_ms - last_alive_log_ms) >= kTaskAliveIntervalMs) {
      ESP_LOGI(kTag,
               "Sampler heartbeat: interval=%" PRIu32 "ms queue=%u",
               interval,
               static_cast<unsigned>(sample_queue_ ? uxQueueMessagesWaiting(sample_queue_) : 0));
      last_alive_log_ms = now_ms;
    }
    // Sleep until next sample slot, but allow command updates to wake this task early.
    (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(interval));
  }
}

void RtuController::publishTask() {
  uint32_t last_alive_log_ms = 0;
  TelemetrySample sample = {};
  while (true) {
    // Block until there is sampled data to publish.
    if (xQueueReceive(sample_queue_, &sample, pdMS_TO_TICKS(200)) != pdTRUE) {
      const uint32_t now_ms = static_cast<uint32_t>(esp_timer_get_time() / 1000ULL);
      if ((now_ms - last_alive_log_ms) >= kTaskAliveIntervalMs) {
        ESP_LOGI(kTag,
                 "Publisher heartbeat: queue=%u backlog=%u connected=%d",
                 static_cast<unsigned>(sample_queue_ ? uxQueueMessagesWaiting(sample_queue_) : 0),
                 static_cast<unsigned>(backlog_.countLines()),
                 net_.isConnected() ? 1 : 0);
        last_alive_log_ms = now_ms;
      }
      continue;
    }
    const std::string payload = sampleToJson(sample);

    if (net_.publishTelemetry(payload)) {
      markPublishResult(true);
      ESP_LOGI(kTag,
               "Telemetry uploaded: seq=%" PRIu32 " (backlog=%u)",
               sample.seq,
               static_cast<unsigned>(backlog_.countLines()));
      const uint32_t now_ms = static_cast<uint32_t>(esp_timer_get_time() / 1000ULL);
      if ((now_ms - last_alive_log_ms) >= kTaskAliveIntervalMs) {
        ESP_LOGI(kTag,
                 "Publisher heartbeat: queue=%u backlog=%u connected=%d",
                 static_cast<unsigned>(sample_queue_ ? uxQueueMessagesWaiting(sample_queue_) : 0),
                 static_cast<unsigned>(backlog_.countLines()),
                 net_.isConnected() ? 1 : 0);
        last_alive_log_ms = now_ms;
      }
      continue;
    }
    markPublishResult(false);

    if (backlog_.appendLine(payload)) {
      ESP_LOGW(kTag, "Offline, cached (%u lines)", static_cast<unsigned>(backlog_.countLines()));
    } else {
      ESP_LOGE(kTag, "Failed to cache sample");
    }

    const uint32_t now_ms = static_cast<uint32_t>(esp_timer_get_time() / 1000ULL);
    if ((now_ms - last_alive_log_ms) >= kTaskAliveIntervalMs) {
      ESP_LOGI(kTag,
               "Publisher heartbeat: queue=%u backlog=%u connected=%d",
               static_cast<unsigned>(sample_queue_ ? uxQueueMessagesWaiting(sample_queue_) : 0),
               static_cast<unsigned>(backlog_.countLines()),
               net_.isConnected() ? 1 : 0);
      last_alive_log_ms = now_ms;
    }
  }
}

void RtuController::connectivityTask() {
  uint32_t last_alive_log_ms = 0;
  uint32_t last_flush_attempt_ms = 0;
  uint32_t flush_log_batch_count = 0;
  sync_window_start_ms_ = static_cast<uint32_t>(esp_timer_get_time() / 1000ULL);
  while (true) {
    // Drive connectivity + backlog housekeeping.
    updateStatusRgb();

    const bool wifi_connected = net_.isConnected();
    const uint32_t now_ms = static_cast<uint32_t>(esp_timer_get_time() / 1000ULL);
    const bool cloud_degraded = isCloudDegraded(now_ms);

    if (!state_event_initialized_) {
      last_wifi_connected_ = wifi_connected;
      last_cloud_degraded_ = cloud_degraded;
      state_event_initialized_ = true;
      if (wifi_connected) {
        char ip_str[32];
        int rssi = 0;
        char meta[128];
        if (readWifiLinkMeta(ip_str, sizeof(ip_str), &rssi)) {
          std::snprintf(meta, sizeof(meta), "{\"ip\":\"%s\",\"rssi\":%d}", ip_str, rssi);
          publishStatusEvent("online", meta);
        } else {
          publishStatusEvent("online");
        }
      } else {
        publishStatusEvent("offline", "{\"reason\":\"wifi_disconnected\"}");
      }
      if (cloud_degraded) {
        publishStatusEvent("cloud_degraded");
      }
    } else {
      if (wifi_connected != last_wifi_connected_) {
        if (wifi_connected) {
          char ip_str[32];
          int rssi = 0;
          char meta[128];
          if (readWifiLinkMeta(ip_str, sizeof(ip_str), &rssi)) {
            std::snprintf(meta, sizeof(meta), "{\"ip\":\"%s\",\"rssi\":%d}", ip_str, rssi);
            publishStatusEvent("online", meta);
          } else {
            publishStatusEvent("online");
          }
        } else {
          publishStatusEvent("offline", "{\"reason\":\"wifi_disconnected\"}");
        }
        last_wifi_connected_ = wifi_connected;
      }
      if (cloud_degraded != last_cloud_degraded_) {
        publishStatusEvent(cloud_degraded ? "cloud_degraded" : "cloud_recovered");
        last_cloud_degraded_ = cloud_degraded;
      }
    }

    // Drain one backlog row per iteration to avoid monopolizing network time.
    const bool can_attempt_flush =
        (now_ms - last_flush_attempt_ms) >= appcfg::BACKLOG_FLUSH_INTERVAL_MS;
    const bool prioritize_live_telemetry =
        sample_queue_ && uxQueueMessagesWaiting(sample_queue_) > 0;
    if (wifi_connected && backlog_.countLines() > 0 && can_attempt_flush && !prioritize_live_telemetry) {
      last_flush_attempt_ms = now_ms;
      std::string row;
      if (backlog_.popOldestLine(row)) {
        if (net_.publishTelemetry(row)) {
          markPublishResult(true);
          ++sync_records_in_window_;
          ++flush_log_batch_count;
          const uint32_t remaining = static_cast<uint32_t>(backlog_.countLines());
          if (flush_log_batch_count >= 10 || remaining <= 10) {
            ESP_LOGI(kTag,
                     "Backlog replay: sent %u rows, %u remaining",
                     static_cast<unsigned>(flush_log_batch_count),
                     static_cast<unsigned>(remaining));
            flush_log_batch_count = 0;
          }
          if (remaining == 0) {
            ESP_LOGI(kTag, "Backlog replay complete");
          }
          const uint32_t now_sync_ms = static_cast<uint32_t>(esp_timer_get_time() / 1000ULL);
          if ((now_sync_ms - sync_window_start_ms_) >= kSyncSummaryIntervalMs ||
              sync_records_in_window_ >= kSyncSummaryMaxRecords) {
            const uint32_t duration_ms = static_cast<uint32_t>(now_sync_ms - sync_window_start_ms_);
            char sync_meta[160];
            std::snprintf(sync_meta,
                          sizeof(sync_meta),
                          "{\"records\":%u,\"duration_ms\":%u}",
                          static_cast<unsigned>(sync_records_in_window_),
                          static_cast<unsigned>(duration_ms));
            publishStatusEvent("data_sync", sync_meta);
            sync_window_start_ms_ = now_sync_ms;
            sync_records_in_window_ = 0;
          }
        } else {
          markPublishResult(false);
          backlog_.prependLine(row);
          vTaskDelay(pdMS_TO_TICKS(500));
        }
      }
    }

    const uint32_t now_hb_ms = static_cast<uint32_t>(esp_timer_get_time() / 1000ULL);
    if ((now_hb_ms - last_heartbeat_ms_) >= kHeartbeatIntervalMs) {
      const bool degraded = isCloudDegraded(now_hb_ms);
      char hb_meta[64];
      std::snprintf(hb_meta,
                    sizeof(hb_meta),
                    "{\"status\":\"%s\"}",
                    degraded ? "degraded" : "healthy");
      publishStatusEvent("heartbeat", hb_meta);
      last_heartbeat_ms_ = now_hb_ms;
    }

    if ((now_hb_ms - last_alive_log_ms) >= kTaskAliveIntervalMs) {
      ESP_LOGI(kTag,
               "Connectivity heartbeat: wifi=%d cloud_degraded=%d backlog=%u",
               net_.isConnected() ? 1 : 0,
               isCloudDegraded(now_hb_ms) ? 1 : 0,
               static_cast<unsigned>(backlog_.countLines()));
      last_alive_log_ms = now_hb_ms;
    }
    vTaskDelay(pdMS_TO_TICKS(150));
  }
}

void RtuController::commandTask() {
  while (true) {
    // Isolated command poll path keeps command cadence independent from backlog flushing.
    net_.commandLoop();
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

void RtuController::onCommand(const std::string& json) {
  // Commands are expected as JSON object. Supported shapes:
  // 1) {"type":"set_sampling_interval","sampling_interval_ms":300000}
  // 2) {"type":"play_buzzer","frequency":1500,"duration":300}
  // 3) {"type":"toggle_led_blink","enabled":true}
  // Legacy compatibility:
  // 4) {"sampling_interval_ms":10000}
  // 5) {"buzzer":{"on":true,"frequency_hz":1200}}
  cJSON* doc = cJSON_Parse(json.c_str());
  if (!doc) {
    ESP_LOGW(kTag, "Invalid command JSON");
    publishStatusEvent("command_failed", "{\"reason\":\"invalid_json\"}");
    return;
  }

  bool ok = false;
  std::string applied_type;
  std::string reason = "unsupported_command";

  cJSON* type_item = cJSON_GetObjectItem(doc, "type");
  const char* type = (cJSON_IsString(type_item) && type_item->valuestring) ? type_item->valuestring : nullptr;

  if (type && std::strcmp(type, "set_sampling_interval") == 0) {
    cJSON* sampling = cJSON_GetObjectItem(doc, "sampling_interval_ms");
    if (cJSON_IsNumber(sampling) && sampling->valueint > 0) {
      const uint32_t bounded = static_cast<uint32_t>(
          std::clamp(sampling->valueint,
                     static_cast<int>(appcfg::MIN_SAMPLING_INTERVAL_MS),
                     static_cast<int>(appcfg::MAX_SAMPLING_INTERVAL_MS)));
      if (cfg_lock_ && xSemaphoreTake(cfg_lock_, pdMS_TO_TICKS(50)) == pdTRUE) {
        const uint32_t previous = sample_interval_ms_;
        sample_interval_ms_ = bounded;
        xSemaphoreGive(cfg_lock_);
        if (sample_task_handle_) {
          xTaskNotifyGive(sample_task_handle_);
        }
        ok = true;
        applied_type = "set_sampling_interval";
        reason = "applied";
        ESP_LOGI(kTag,
                 "Command set_sampling_interval: %" PRIu32 "ms -> %" PRIu32 "ms (sampler wake requested)",
                 previous,
                 bounded);
      } else {
        reason = "config_lock_timeout";
      }
    } else {
      reason = "invalid_sampling_interval_ms";
    }
  } else if (type && std::strcmp(type, "play_buzzer") == 0) {
    cJSON* freq = cJSON_GetObjectItem(doc, "frequency");
    cJSON* duration = cJSON_GetObjectItem(doc, "duration");
    const uint16_t tone_f = cJSON_IsNumber(freq)
                                ? static_cast<uint16_t>(std::clamp(freq->valueint, 200, 5000))
                                : kToneDefaultFreqHz;
    const uint32_t tone_d = cJSON_IsNumber(duration) && duration->valueint >= 0
                                ? static_cast<uint32_t>(std::clamp(duration->valueint, 50, 5000))
                                : kToneDefaultDurationMs;
    if (actuator_.playTone(tone_f, tone_d) == ESP_OK) {
      ok = true;
      applied_type = "play_buzzer";
      reason = "applied";
      ESP_LOGI(kTag, "Command play_buzzer freq=%u duration_ms=%u", tone_f, tone_d);
    } else {
      reason = "actuator_buzzer_failed";
    }
  } else if (type &&
             (std::strcmp(type, "toggle_led_blink") == 0 ||
              std::strcmp(type, "set_led_blink") == 0 ||
              std::strcmp(type, "blink_led") == 0)) {
    cJSON* enabled = cJSON_GetObjectItem(doc, "enabled");
    if (!enabled) enabled = cJSON_GetObjectItem(doc, "on");
    if (!enabled) enabled = cJSON_GetObjectItem(doc, "blink");
    if (cJSON_IsBool(enabled) || cJSON_IsNumber(enabled)) {
      const bool next = cJSON_IsBool(enabled) ? cJSON_IsTrue(enabled) : (enabled->valueint != 0);
      led_blink_enabled_ = next;
      ok = true;
      applied_type = "toggle_led_blink";
      reason = "applied";
      ESP_LOGI(kTag, "Command toggle_led_blink enabled=%d", next ? 1 : 0);
    } else {
      reason = "invalid_blink_enabled";
    }
  } else {
    // Legacy compatibility: direct sampling_interval_ms without "type".
    cJSON* sampling = cJSON_GetObjectItem(doc, "sampling_interval_ms");
    if (cJSON_IsNumber(sampling) && sampling->valueint > 0) {
      const uint32_t bounded = static_cast<uint32_t>(
          std::clamp(sampling->valueint,
                     static_cast<int>(appcfg::MIN_SAMPLING_INTERVAL_MS),
                     static_cast<int>(appcfg::MAX_SAMPLING_INTERVAL_MS)));
      if (cfg_lock_ && xSemaphoreTake(cfg_lock_, pdMS_TO_TICKS(50)) == pdTRUE) {
        const uint32_t previous = sample_interval_ms_;
        sample_interval_ms_ = bounded;
        xSemaphoreGive(cfg_lock_);
        if (sample_task_handle_) {
          xTaskNotifyGive(sample_task_handle_);
        }
        ok = true;
        applied_type = "set_sampling_interval";
        reason = "applied_legacy";
        ESP_LOGI(kTag,
                 "Legacy command set_sampling_interval: %" PRIu32 "ms -> %" PRIu32 "ms (sampler wake requested)",
                 previous,
                 bounded);
      } else {
        reason = "config_lock_timeout";
      }
    } else {
      // Legacy compatibility: buzzer object.
      cJSON* buzzer = cJSON_GetObjectItem(doc, "buzzer");
      if (buzzer && cJSON_IsObject(buzzer)) {
        cJSON* on = cJSON_GetObjectItem(buzzer, "on");
        cJSON* freq = cJSON_GetObjectItem(buzzer, "frequency_hz");
        const bool buzzer_on = cJSON_IsBool(on) ? cJSON_IsTrue(on) : false;
        const int f = cJSON_IsNumber(freq) ? freq->valueint : static_cast<int>(appcfg::PIEZO_DEFAULT_FREQ_HZ);
        const uint16_t bounded_f = static_cast<uint16_t>(std::clamp(f, 200, 5000));
        if (actuator_.setBuzzer(buzzer_on, bounded_f) == ESP_OK) {
          ok = true;
          applied_type = "legacy_buzzer";
          reason = "applied_legacy";
          ESP_LOGI(kTag, "Legacy command buzzer=%d freq=%u", buzzer_on ? 1 : 0, bounded_f);
        } else {
          reason = "actuator_buzzer_failed";
        }
      }
    }
  }

  cJSON_Delete(doc);

  char meta[256];
  if (ok) {
    // A successfully handled command implies cloud connectivity is currently healthy.
    markPublishResult(true);
    std::snprintf(meta,
                  sizeof(meta),
                  "{\"result\":\"pass\",\"type\":\"%s\"}",
                  applied_type.empty() ? "unknown" : applied_type.c_str());
    publishStatusEvent("command_applied", meta);
    return;
  }

  std::snprintf(meta,
                sizeof(meta),
                "{\"result\":\"fail\",\"reason\":\"%s\",\"type\":\"%s\"}",
                reason.c_str(),
                type ? type : "unknown");
  publishStatusEvent("command_failed", meta);
}

void RtuController::publishStatusEvent(const char* event, const std::string& metadata_json) {
  if (!event || event[0] == '\0') return;
  net_.publishStatus(statusToJson(event, metadata_json));
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

std::string RtuController::statusToJson(const char* event, const std::string& metadata_json) const {
  const uint64_t uptime_ms = static_cast<uint64_t>(esp_timer_get_time() / 1000ULL);
  cJSON* root = cJSON_CreateObject();
  if (!root) return "{}";
  cJSON_AddStringToObject(root, "device_id", appcfg::DEVICE_ID);
  cJSON_AddStringToObject(root, "event", event ? event : "unknown");
  cJSON_AddNumberToObject(root, "uptime_ms", static_cast<double>(uptime_ms));

  cJSON* metadata = cJSON_Parse(metadata_json.c_str());
  if (!metadata || !cJSON_IsObject(metadata)) {
    if (metadata) cJSON_Delete(metadata);
    metadata = cJSON_CreateObject();
  }
  cJSON_AddItemToObject(root, "metadata", metadata);

  char* encoded = cJSON_PrintUnformatted(root);
  std::string out = encoded ? encoded : "{}";
  if (encoded) cJSON_free(encoded);
  cJSON_Delete(root);
  return out;
}

void RtuController::updateStatusRgb() {
  uint8_t r = 0;
  uint8_t g = 0;
  uint8_t b = 0;
  const uint32_t now_ms = static_cast<uint32_t>(esp_timer_get_time() / 1000ULL);

  // Red = station disconnected from Wi-Fi.
  if (!net_.isConnected()) {
    r = 255;
    g = 0;
    b = 0;
  } else if (isCloudDegraded(now_ms)) {
    // Yellow = Wi-Fi is up but cloud publish path is failing/degraded.
    r = 255;
    g = 180;
    b = 0;
  } else {
    uint32_t interval = appcfg::DEFAULT_SAMPLING_INTERVAL_MS;
    if (cfg_lock_ && xSemaphoreTake(cfg_lock_, pdMS_TO_TICKS(10)) == pdTRUE) {
      interval = sample_interval_ms_;
      xSemaphoreGive(cfg_lock_);
    }

    // Sampling-rate color map:
    // - 5 seconds   => magenta (fast)
    // - 5 minutes   => green (default)
    // - 30 minutes  => blue (slow)
    // Any other cadence falls back to green.
    if (interval == kSamplingFastMs) {
      r = 255;
      g = 0;
      b = 255;
    } else if (interval == kSamplingSlowMs) {
      r = 0;
      g = 0;
      b = 255;
    } else if (interval == kSamplingDefaultMs) {
      r = 0;
      g = 255;
      b = 0;
    } else {
      r = 0;
      g = 255;
      b = 0;
    }
  }

  // Optional blink overlay: color is shown for half cycle, then turned off.
  if (led_blink_enabled_) {
    const uint32_t phase_ms = now_ms % kLedBlinkPeriodMs;
    if (phase_ms >= kLedBlinkOnWindowMs) {
      r = 0;
      g = 0;
      b = 0;
    }
  }

  if (actuator_.setRgb(r, g, b) != ESP_OK) {
    ESP_LOGW(kTag, "Status RGB update failed");
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

  // Derive staleness window from current sampling cadence to avoid false yellow state
  // when running intentionally slow sample intervals (for example 5m or 30m presets).
  uint32_t interval_ms = appcfg::DEFAULT_SAMPLING_INTERVAL_MS;
  if (cfg_lock_ && xSemaphoreTake(cfg_lock_, pdMS_TO_TICKS(10)) == pdTRUE) {
    interval_ms = sample_interval_ms_;
    xSemaphoreGive(cfg_lock_);
  }
  const uint32_t interval_scaled = interval_ms > (UINT32_MAX / 2U) ? UINT32_MAX : (interval_ms * 2U);
  const uint32_t stale_threshold_ms =
      interval_scaled > kCloudStaleThresholdMinMs ? interval_scaled : kCloudStaleThresholdMinMs;

  // Degraded if we have historical success but no recent success.
  if (has_publish_success_) {
    return (now_ms - last_publish_ok_ms_) > stale_threshold_ms;
  }
  // During cold boot, do not mark degraded until first success/fail streak evidence exists.
  return false;
}

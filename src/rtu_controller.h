#pragma once

#include <string>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "actuator_hal.h"
#include "app_config.h"
#include "app_types.h"
#include "backlog_store.h"
#include "network_hal.h"
#include "sensor_hal.h"

/*
 * RtuController is the orchestration layer for the entire firmware.
 *
 * Runtime model:
 * - sampleTask: acquires periodic telemetry from SensorHal and enqueues it
 * - publishTask: publishes queue items to cloud or stores them in backlog
 * - connectivityTask: flushes backlog and maintains status events / RGB state
 * - commandTask: polls cloud commands and dispatches to onCommand()
 *
 * Design goal:
 * - keep business flow in one place while leaving hardware/protocol details in HALs
 */
class RtuController {
 public:
  RtuController();
  // Performs one-time startup. Safe against duplicate calls.
  void begin();
  // Lightweight top-level idle loop called from app_main().
  void loop();
  bool isStarted() const { return started_; }

 private:
  static void sampleTaskThunk(void* ctx);
  static void publishTaskThunk(void* ctx);
  static void connectivityTaskThunk(void* ctx);
  static void commandTaskThunk(void* ctx);
  void updateStatusRgb();
  void sampleTask();
  void publishTask();
  void connectivityTask();
  void commandTask();
  void onCommand(const std::string& json);
  // Publish a status-table event row for lifecycle/health observability.
  void publishStatusEvent(const char* event, const std::string& metadata_json = "{}");
  // Serialize one sample row for Supabase REST insert.
  std::string sampleToJson(const TelemetrySample& sample) const;
  // Serialize status event row for Supabase REST insert.
  std::string statusToJson(const char* event, const std::string& metadata_json) const;
  // Tracks rolling cloud publish health for LED status decisions.
  void markPublishResult(bool ok);
  // Returns true when cloud path is considered degraded (Wi-Fi may still be up).
  bool isCloudDegraded(uint32_t now_ms) const;

  SensorHal sensor_;
  ActuatorHal actuator_;
  NetworkHal net_;
  std::string device_id_;
  BacklogStore backlog_;

  QueueHandle_t sample_queue_ = nullptr;
  TaskHandle_t sample_task_handle_ = nullptr;
  SemaphoreHandle_t cfg_lock_ = nullptr;
  // Sampling interval may be changed by remote commands; protected by cfg_lock_.
  volatile uint32_t sample_interval_ms_ = appcfg::DEFAULT_SAMPLING_INTERVAL_MS;
  uint32_t sequence_ = 0;
  bool started_ = false;
  // Cloud health tracking:
  // - last successful publish timestamp
  // - consecutive failure streak
  // - bootstrap guard for pre-success state
  uint32_t last_publish_ok_ms_ = 0;
  uint32_t publish_fail_streak_ = 0;
  bool has_publish_success_ = false;
  // Transition tracking for status-event emission (avoid per-loop spam).
  bool state_event_initialized_ = false;
  bool last_wifi_connected_ = false;
  bool last_cloud_degraded_ = false;
  bool calibration_reported_ = false;
  uint32_t last_heartbeat_ms_ = 0;
  uint32_t sync_window_start_ms_ = 0;
  uint32_t sync_records_in_window_ = 0;
  // When true, status LED color blinks on/off using the currently selected status color.
  bool led_blink_enabled_ = false;
};

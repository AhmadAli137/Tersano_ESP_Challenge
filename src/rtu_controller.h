#pragma once

#include <string>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

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
 * - connectivityTask: maintains network loop, flushes backlog, updates status LED
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
  void updateStatusRgb();
  void sampleTask();
  void publishTask();
  void connectivityTask();
  void onCommand(const std::string& json);
  // Serialize one sample row for Supabase REST insert.
  std::string sampleToJson(const TelemetrySample& sample) const;
  // Serialize status event row for Supabase REST insert.
  std::string statusToJson(const char* event) const;
  // Tracks rolling cloud publish health for LED status decisions.
  void markPublishResult(bool ok);
  // Returns true when cloud path is considered degraded (Wi-Fi may still be up).
  bool isCloudDegraded(uint32_t now_ms) const;

  SensorHal sensor_;
  ActuatorHal actuator_;
  NetworkHal net_;
  BacklogStore backlog_;

  QueueHandle_t sample_queue_ = nullptr;
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
};

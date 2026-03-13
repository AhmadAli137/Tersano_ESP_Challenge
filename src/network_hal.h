#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "app_config.h"

/*
 * NetworkHal owns connectivity + cloud transport:
 * - Wi-Fi station lifecycle
 * - HTTPS REST requests to Supabase
 * - command polling callback dispatch
 *
 * Threading note:
 * - publish/status/command-loop may be called from different RTU tasks.
 * - request helper serializes HTTP operations with a mutex.
 */
class NetworkHal {
 public:
  using CommandHandler = std::function<void(const std::string&)>;

  NetworkHal(const char* wifi_ssid,
             const char* wifi_pass,
             const char* supabase_url,
             const char* supabase_api_key,
             const char* telemetry_table,
             const char* commands_table,
             const char* status_table,
             const char* device_id);

  // Initialize Wi-Fi and networking stack once.
  void begin();
  // Backward-compatible periodic worker alias.
  void loop();
  // Periodic command poll worker based on configured interval.
  void commandLoop();
  // True when station has acquired IP and link is usable.
  bool isConnected() const;
  // POST one telemetry payload to Supabase REST endpoint.
  bool publishTelemetry(const std::string& payload);
  // POST one status payload to Supabase REST endpoint.
  bool publishStatus(const std::string& payload);
  // Register command callback invoked with JSON command payload.
  void setCommandHandler(CommandHandler cb) { command_handler_ = cb; }

 private:
  // Generic HTTP helper for GET/POST/PATCH against Supabase.
  bool httpRequest(const char* method,
                   const std::string& path_or_query,
                   const char* content_type,
                   const std::string& body,
                   int& status_code,
                   std::string* response_body);
  // Pull latest unprocessed command for this device.
  bool pollCommand(bool* had_pending_command);
  // Mark command row as processed after callback succeeds.
  bool markCommandProcessed(const std::string& command_id);
  // Build /rest/v1/<table> style path.
  std::string buildRestPath(const char* table) const;
  // Join base Supabase URL with absolute or relative path/query.
  std::string fullUrl(const std::string& path_or_query) const;

  // Injected credentials and table identifiers.
  const char* wifi_ssid_;
  const char* wifi_pass_;
  const char* supabase_url_;
  const char* supabase_api_key_;
  const char* telemetry_table_;
  const char* commands_table_;
  const char* status_table_;
  const char* device_id_;

  CommandHandler command_handler_;
  // Last command-poll timestamp in milliseconds since boot.
  uint32_t last_command_poll_ms_ = 0;
  // Adaptive command poll interval (starts at base, grows while no commands are pending).
  uint32_t command_poll_interval_ms_ = appcfg::COMMAND_POLL_INTERVAL_MS;
  // Guards esp_http_client usage so publish/poll/flush do not run concurrent HTTP operations.
  SemaphoreHandle_t http_lock_ = nullptr;
};

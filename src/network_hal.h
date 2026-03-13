#pragma once

#include <cstdint>
#include <functional>
#include <string>

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

  void begin();
  void loop();
  bool isConnected() const;
  bool publishTelemetry(const std::string& payload);
  bool publishStatus(const std::string& payload);
  void setCommandHandler(CommandHandler cb) { command_handler_ = cb; }

 private:
  bool httpRequest(const char* method,
                   const std::string& path_or_query,
                   const char* content_type,
                   const std::string& body,
                   int& status_code,
                   std::string* response_body);
  bool pollCommand();
  bool markCommandProcessed(uint64_t command_id);
  std::string buildRestPath(const char* table) const;
  std::string fullUrl(const std::string& path_or_query) const;

  const char* wifi_ssid_;
  const char* wifi_pass_;
  const char* supabase_url_;
  const char* supabase_api_key_;
  const char* telemetry_table_;
  const char* commands_table_;
  const char* status_table_;
  const char* device_id_;

  CommandHandler command_handler_;
  uint32_t last_command_poll_ms_ = 0;
};

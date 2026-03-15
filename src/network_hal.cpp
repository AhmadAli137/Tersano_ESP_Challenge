#include "network_hal.h"

#include <cstring>

#include "cJSON.h"
#include "esp_event.h"
#include "esp_err.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_crt_bundle.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "app_config.h"

namespace {
constexpr const char* kTag = "NetworkHal";
constexpr int kWifiConnectedBit = BIT0;
constexpr size_t kStatusPayloadLogLimit = 240;
constexpr size_t kHttpResponseCaptureLimit = 2048;
constexpr uint32_t kNoCommandLogIntervalMs = 60000;
constexpr uint32_t kCommandAckRetryCount = 3;
constexpr uint32_t kCommandAckRetryBaseMs = 250;
constexpr uint32_t kCommandPollBackoffStepMs = 500;
constexpr uint32_t kCommandPollBackoffMaxMs = 5000;
// Process-wide Wi-Fi event group used by isConnected() and event callbacks.
EventGroupHandle_t g_wifi_event_group = nullptr;
bool g_wifi_ready = false;
uint32_t g_last_no_command_log_ms = 0;

esp_err_t httpEventHandler(esp_http_client_event_t* evt) {
  if (!evt) return ESP_OK;
  if (evt->event_id == HTTP_EVENT_ON_DATA && evt->user_data && evt->data && evt->data_len > 0) {
    auto* out = static_cast<std::string*>(evt->user_data);
    const size_t incoming = static_cast<size_t>(evt->data_len);
    if (out->size() >= kHttpResponseCaptureLimit) return ESP_OK;
    const size_t room = kHttpResponseCaptureLimit - out->size();
    const size_t take = incoming < room ? incoming : room;
    if (take > 0) {
      out->append(static_cast<const char*>(evt->data), take);
    }
  }
  return ESP_OK;
}

// Shared event callback for Wi-Fi + IP state transitions.
void wifiEventHandler(void*,
                      esp_event_base_t event_base,
                      int32_t event_id,
                      void* event_data) {
  if (!g_wifi_event_group) return;
  // Station lifecycle events:
  // - on start: connect
  // - on disconnect: clear bit + reconnect
  // - on got IP: set connected bit
  if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
    ESP_LOGI(kTag, "Wi-Fi station started, attempting connection...");
    esp_wifi_connect();
  } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
    auto* disc = static_cast<wifi_event_sta_disconnected_t*>(event_data);
    const int reason = disc ? static_cast<int>(disc->reason) : -1;
    ESP_LOGW(kTag, "Wi-Fi disconnected (reason=%d), reconnecting...", reason);
    xEventGroupClearBits(g_wifi_event_group, kWifiConnectedBit);
    esp_wifi_connect();
  } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
    auto* got_ip = static_cast<ip_event_got_ip_t*>(event_data);
    if (got_ip) {
      ESP_LOGI(kTag,
               "Wi-Fi connected. IP address acquired: " IPSTR,
               IP2STR(&got_ip->ip_info.ip));
    } else {
      ESP_LOGI(kTag, "Wi-Fi connected. IP address acquired.");
    }
    xEventGroupSetBits(g_wifi_event_group, kWifiConnectedBit);
  }
}
}  // namespace

NetworkHal::NetworkHal(const char* wifi_ssid,
                       const char* wifi_pass,
                       const char* supabase_url,
                       const char* supabase_api_key,
                       const char* telemetry_table,
                       const char* commands_table,
                       const char* status_table)
    : wifi_ssid_(wifi_ssid),
      wifi_pass_(wifi_pass),
      supabase_url_(supabase_url),
      supabase_api_key_(supabase_api_key),
      telemetry_table_(telemetry_table),
      commands_table_(commands_table),
      status_table_(status_table) {}

void NetworkHal::begin() {
  if (!g_wifi_ready) {
    ESP_LOGI(kTag, "Initializing network stack...");
    // NVS is required by Wi-Fi stack; recover from partition layout/version changes.
    esp_err_t nvs_rc = nvs_flash_init();
    if (nvs_rc == ESP_ERR_NVS_NO_FREE_PAGES || nvs_rc == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      nvs_flash_erase();
      nvs_flash_init();
    }
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();

    // Bring up Wi-Fi driver + event handling.
    // NetworkHal deliberately keeps station setup simple and robust.
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);
    g_wifi_event_group = xEventGroupCreate();
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifiEventHandler, nullptr);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifiEventHandler, nullptr);

    wifi_config_t wifi_cfg = {};
    std::strncpy(reinterpret_cast<char*>(wifi_cfg.sta.ssid), wifi_ssid_, sizeof(wifi_cfg.sta.ssid) - 1);
    std::strncpy(reinterpret_cast<char*>(wifi_cfg.sta.password), wifi_pass_, sizeof(wifi_cfg.sta.password) - 1);
    wifi_cfg.sta.pmf_cfg.capable = true;
    wifi_cfg.sta.pmf_cfg.required = false;

    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg);
    esp_wifi_start();
    ESP_LOGI(kTag, "Command table configured as: %s", commands_table_);
    http_lock_ = xSemaphoreCreateMutex();
    if (!http_lock_) {
      ESP_LOGE(kTag, "Failed to create HTTP mutex");
    }
    g_wifi_ready = true;
    ESP_LOGI(kTag, "Network stack initialized");
  }
}

void NetworkHal::loop() {
  commandLoop();
}

void NetworkHal::commandLoop() {
  if (!isConnected()) return;
  const uint32_t now = static_cast<uint32_t>(esp_timer_get_time() / 1000ULL);
  // Poll commands at controlled cadence to avoid excessive backend traffic.
  if (now - last_command_poll_ms_ >= command_poll_interval_ms_) {
    last_command_poll_ms_ = now;
    bool had_pending_command = false;
    const bool ok = pollCommand(&had_pending_command);
    if (!ok) {
      // Keep responsive retries on transport/HTTP failures.
      command_poll_interval_ms_ = appcfg::COMMAND_POLL_INTERVAL_MS;
      return;
    }
    if (had_pending_command) {
      // A command is flowing: stay at base interval for low-latency follow-up commands.
      command_poll_interval_ms_ = appcfg::COMMAND_POLL_INTERVAL_MS;
      return;
    }
    // No pending rows: gradually back off to reduce load while idle.
    if (command_poll_interval_ms_ < kCommandPollBackoffMaxMs) {
      const uint32_t next = command_poll_interval_ms_ + kCommandPollBackoffStepMs;
      command_poll_interval_ms_ =
          next > kCommandPollBackoffMaxMs ? kCommandPollBackoffMaxMs : next;
    }
  }
}

bool NetworkHal::isConnected() const {
  if (!g_wifi_event_group) return false;
  return (xEventGroupGetBits(g_wifi_event_group) & kWifiConnectedBit) != 0;
}

bool NetworkHal::publishTelemetry(const std::string& payload) {
  int status = 0;
  if (!isConnected()) {
    ESP_LOGW(kTag, "Telemetry publish skipped: Wi-Fi not connected");
    return false;
  }
  if (payload.empty()) {
    ESP_LOGW(kTag, "Telemetry publish skipped: empty payload");
    return false;
  }
  const int preview_len = static_cast<int>(
      payload.size() < kStatusPayloadLogLimit ? payload.size() : kStatusPayloadLogLimit);
  if (!httpRequest("POST", buildRestPath(telemetry_table_), "application/json", payload, status, nullptr)) {
    ESP_LOGW(kTag,
             "Telemetry publish failed: transport/TLS error payload(%uB)=%.*s%s",
             static_cast<unsigned>(payload.size()),
             preview_len,
             payload.c_str(),
             payload.size() > kStatusPayloadLogLimit ? "...<truncated>" : "");
    return false;
  }
  if (status < 200 || status >= 300) {
    ESP_LOGW(kTag,
             "Telemetry publish failed: HTTP status=%d payload(%uB)=%.*s%s",
             status,
             static_cast<unsigned>(payload.size()),
             preview_len,
             payload.c_str(),
             payload.size() > kStatusPayloadLogLimit ? "...<truncated>" : "");
    return false;
  }
  return true;
}

bool NetworkHal::publishStatus(const std::string& payload) {
  int status = 0;
  if (!isConnected()) {
    ESP_LOGW(kTag, "Status publish skipped: Wi-Fi not connected");
    return false;
  }
  if (!httpRequest("POST", buildRestPath(status_table_), "application/json", payload, status, nullptr)) {
    const int preview_len = static_cast<int>(
        payload.size() < kStatusPayloadLogLimit ? payload.size() : kStatusPayloadLogLimit);
    ESP_LOGW(kTag,
             "Status publish failed: transport/TLS error payload(%uB)=%.*s%s",
             static_cast<unsigned>(payload.size()),
             preview_len,
             payload.c_str(),
             payload.size() > kStatusPayloadLogLimit ? "...<truncated>" : "");
    return false;
  }
  if (status < 200 || status >= 300) {
    const int preview_len = static_cast<int>(
        payload.size() < kStatusPayloadLogLimit ? payload.size() : kStatusPayloadLogLimit);
    ESP_LOGW(kTag,
             "Status publish failed: HTTP status=%d payload(%uB)=%.*s%s",
             status,
             static_cast<unsigned>(payload.size()),
             preview_len,
             payload.c_str(),
             payload.size() > kStatusPayloadLogLimit ? "...<truncated>" : "");
    return false;
  }
  return true;
}

bool NetworkHal::httpRequest(const char* method,
                             const std::string& path_or_query,
                             const char* content_type,
                             const std::string& body,
                             int& status_code,
                             std::string* response_body) {
  if (!http_lock_) {
    ESP_LOGW(kTag, "HTTP request skipped: network lock not initialized");
    return false;
  }
  // Serialize requests from multiple tasks (publish, flush, command poll) to avoid client contention.
  if (xSemaphoreTake(http_lock_, pdMS_TO_TICKS(4000)) != pdTRUE) {
    ESP_LOGW(kTag, "HTTP request skipped: network lock timeout");
    return false;
  }

  // Build one HTTPS client per request.
  // NOTE: In current architecture this function can be called from multiple tasks.
  // Per-call client handles keep this path re-entrant and avoid shared-state races.
  // We still enable TCP keep-alive so underlying transport can reuse sockets when possible.
  std::string local_body;
  std::string* body_out = response_body ? response_body : &local_body;
  body_out->clear();
  body_out->reserve(512);

  esp_http_client_config_t cfg = {};
  const std::string url = fullUrl(path_or_query);
  cfg.url = url.c_str();
  // Keep blocking network calls short enough to avoid starving the single-core scheduler.
  cfg.timeout_ms = 3000;
  // Supabase (via edge/CDN) can return large header sets; default buffers are often too small.
  cfg.buffer_size = 4096;
  cfg.buffer_size_tx = 2048;
  cfg.keep_alive_enable = true;
  // Do not let esp_http_client attempt WWW-Authenticate negotiation (it doesn't support Bearer).
  // We provide auth headers explicitly and want raw HTTP status/body on auth failures.
  cfg.max_authorization_retries = 0;
  // Avoid HTTP auth negotiation paths ("Bearer challenge") and treat this as plain REST + API key.
  cfg.auth_type = HTTP_AUTH_TYPE_NONE;
  cfg.transport_type = HTTP_TRANSPORT_OVER_SSL;
  cfg.crt_bundle_attach = esp_crt_bundle_attach;
  cfg.user_data = body_out;
  cfg.event_handler = httpEventHandler;

  esp_http_client_handle_t client = esp_http_client_init(&cfg);
  if (!client) {
    xSemaphoreGive(http_lock_);
    return false;
  }
  esp_http_client_set_method(client, !std::strcmp(method, "GET") ? HTTP_METHOD_GET
                                      : !std::strcmp(method, "PATCH") ? HTTP_METHOD_PATCH
                                                                      : HTTP_METHOD_POST);
  // Supabase REST auth headers:
  // - apikey is always required
  // - Authorization Bearer mirrors common PostgREST/Supabase client behavior
  esp_http_client_set_header(client, "apikey", supabase_api_key_);
  std::string auth = std::string("Bearer ") + supabase_api_key_;
  esp_http_client_set_header(client, "Authorization", auth.c_str());
  esp_http_client_set_header(client, "Accept", "application/json");
  if (std::strcmp(method, "GET") != 0) {
    esp_http_client_set_header(client, "Prefer", "return=minimal");
  }
  if (content_type) esp_http_client_set_header(client, "Content-Type", content_type);
  if (!body.empty()) esp_http_client_set_post_field(client, body.c_str(), body.size());

  const esp_err_t err = esp_http_client_perform(client);
  if (err != ESP_OK) {
    ESP_LOGW(kTag,
             "HTTP %s %s failed: %s (0x%x)",
             method,
             url.c_str(),
             esp_err_to_name(err),
             static_cast<unsigned>(err));
    esp_http_client_cleanup(client);
    xSemaphoreGive(http_lock_);
    return false;
  }

  status_code = esp_http_client_get_status_code(client);
  if (status_code >= 400) {
    const char* resp_text = body_out->empty() ? "<empty>" : body_out->c_str();
    ESP_LOGW(kTag, "HTTP %s %s returned status=%d body=%s",
             method, url.c_str(), status_code, resp_text);
  }

  esp_http_client_cleanup(client);
  xSemaphoreGive(http_lock_);
  return true;
}

bool NetworkHal::pollCommand(bool* had_pending_command) {
  if (had_pending_command) *had_pending_command = false;
  if (!command_handler_) return false;
  if (device_id_.empty()) {
    ESP_LOGW(kTag, "Command poll skipped: device_id is not set");
    return false;
  }

  int status = 0;
  std::string response;
  const std::string query = buildRestPath(commands_table_) +
                            "?device_id=eq." + device_id_ +
                            "&processed=eq.false&order=id.asc&limit=1&select=*";

  if (!httpRequest("GET", query, nullptr, "", status, &response)) {
    ESP_LOGW(kTag, "Command poll failed: transport/TLS error");
    return false;
  }
  if (status < 200 || status >= 300) {
    ESP_LOGW(kTag, "Command poll failed: HTTP status=%d", status);
    return false;
  }
  ESP_LOGD(kTag, "Command poll HTTP %d, response bytes=%u",
           status, static_cast<unsigned>(response.size()));

  // Expected response: JSON array with zero or one command row.
  // Empty body is interpreted as "no command available".
  if (response.empty()) {
    // Some backends/proxies may return 204/empty for "no rows"; treat as no command.
    const uint32_t now_ms = static_cast<uint32_t>(esp_timer_get_time() / 1000ULL);
    if ((now_ms - g_last_no_command_log_ms) >= kNoCommandLogIntervalMs) {
      ESP_LOGI(kTag, "Command poll active: no pending command rows (empty response)");
      g_last_no_command_log_ms = now_ms;
    }
    return true;
  }
  cJSON* root = cJSON_Parse(response.c_str());
  if (!root) {
    ESP_LOGW(kTag, "Command poll failed: response was not valid JSON (body=%s)",
             response.empty() ? "<empty>" : response.c_str());
    return false;
  }
  if (!cJSON_IsArray(root) || cJSON_GetArraySize(root) == 0) {
    cJSON_Delete(root);
    const uint32_t now_ms = static_cast<uint32_t>(esp_timer_get_time() / 1000ULL);
    if ((now_ms - g_last_no_command_log_ms) >= kNoCommandLogIntervalMs) {
      ESP_LOGI(kTag, "Command poll active: no pending command rows");
      g_last_no_command_log_ms = now_ms;
    }
    return true;
  }

  cJSON* row = cJSON_GetArrayItem(root, 0);
  if (had_pending_command) *had_pending_command = true;
  cJSON* id = cJSON_GetObjectItem(row, "id");
  cJSON* cmd = cJSON_GetObjectItem(row, "command");
  if (!cmd) {
    // Compatibility path: some schemas use "payload" instead of "command".
    cmd = cJSON_GetObjectItem(row, "payload");
  }
  if (!id || !cmd) {
    cJSON_Delete(root);
    ESP_LOGW(kTag, "Command poll failed: malformed command row (expected id + command/payload)");
    return false;
  }

  char* cmd_text = cJSON_PrintUnformatted(cmd);
  if (!cmd_text) {
    cJSON_Delete(root);
    ESP_LOGW(kTag, "Command poll failed: JSON serialization error");
    return false;
  }
  std::string cmd_id;
  if (cJSON_IsString(id) && id->valuestring) {
    cmd_id = id->valuestring;
  } else if (cJSON_IsNumber(id)) {
    // Backward compatibility with integer id schemas.
    char id_buf[32];
    std::snprintf(id_buf, sizeof(id_buf), "%.0f", id->valuedouble);
    cmd_id = id_buf;
  } else {
    cJSON_Delete(root);
    ESP_LOGW(kTag, "Command poll failed: unsupported id type");
    return false;
  }

  const std::string cmd_payload(cmd_text);
  cJSON_free(cmd_text);
  const int cmd_preview_len = static_cast<int>(
      cmd_payload.size() < kStatusPayloadLogLimit ? cmd_payload.size() : kStatusPayloadLogLimit);
  ESP_LOGI(kTag,
           "Command received id=%s payload(%uB)=%.*s%s",
           cmd_id.c_str(),
           static_cast<unsigned>(cmd_payload.size()),
           cmd_preview_len,
           cmd_payload.c_str(),
           cmd_payload.size() > kStatusPayloadLogLimit ? "...<truncated>" : "");
  command_handler_(cmd_payload);
  cJSON_Delete(root);
  const bool marked = markCommandProcessed(cmd_id);
  if (marked) {
    ESP_LOGI(kTag, "Command processed id=%s", cmd_id.c_str());
  } else {
    ESP_LOGW(kTag, "Command process ACK failed id=%s", cmd_id.c_str());
  }
  return marked;
}

bool NetworkHal::markCommandProcessed(const std::string& command_id) {
  if (command_id.empty()) {
    ESP_LOGW(kTag, "Mark command processed failed: empty id");
    return false;
  }
  const std::string query = buildRestPath(commands_table_) + "?id=eq." + command_id;
  for (uint32_t attempt = 1; attempt <= kCommandAckRetryCount; ++attempt) {
    int status = 0;
    if (httpRequest("PATCH", query, "application/json", "{\"processed\":true}", status, nullptr) &&
        status >= 200 && status < 300) {
      return true;
    }

    ESP_LOGW(kTag,
             "Mark command processed retry %u/%u failed for id=%s",
             static_cast<unsigned>(attempt),
             static_cast<unsigned>(kCommandAckRetryCount),
             command_id.c_str());
    if (attempt < kCommandAckRetryCount) {
      vTaskDelay(pdMS_TO_TICKS(kCommandAckRetryBaseMs * attempt));
    }
  }
  return false;
}

std::string NetworkHal::buildRestPath(const char* table) const {
  return std::string("/rest/v1/") + table;
}

std::string NetworkHal::fullUrl(const std::string& path_or_query) const {
  if (!path_or_query.empty() && path_or_query.front() == '/') return std::string(supabase_url_) + path_or_query;
  return std::string(supabase_url_) + "/" + path_or_query;
}

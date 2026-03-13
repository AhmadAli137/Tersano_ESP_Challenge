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
#include "nvs_flash.h"

#include "app_config.h"

namespace {
constexpr const char* kTag = "NetworkHal";
constexpr int kWifiConnectedBit = BIT0;
constexpr size_t kStatusPayloadLogLimit = 240;
// Process-wide Wi-Fi event group used by isConnected() and event callbacks.
EventGroupHandle_t g_wifi_event_group = nullptr;
bool g_wifi_ready = false;

std::string logPreview(const std::string& text, size_t max_len = kStatusPayloadLogLimit) {
  if (text.size() <= max_len) return text;
  return text.substr(0, max_len) + "...<truncated>";
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
                       const char* status_table,
                       const char* device_id)
    : wifi_ssid_(wifi_ssid),
      wifi_pass_(wifi_pass),
      supabase_url_(supabase_url),
      supabase_api_key_(supabase_api_key),
      telemetry_table_(telemetry_table),
      commands_table_(commands_table),
      status_table_(status_table),
      device_id_(device_id) {}

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
    http_lock_ = xSemaphoreCreateMutex();
    if (!http_lock_) {
      ESP_LOGE(kTag, "Failed to create HTTP mutex");
    }
    g_wifi_ready = true;
    ESP_LOGI(kTag, "Network stack initialized");
  }
}

void NetworkHal::loop() {
  if (!isConnected()) return;
  const uint32_t now = static_cast<uint32_t>(esp_timer_get_time() / 1000ULL);
  // Poll commands at controlled cadence to avoid excessive backend traffic.
  if (now - last_command_poll_ms_ >= appcfg::COMMAND_POLL_INTERVAL_MS) {
    last_command_poll_ms_ = now;
    pollCommand();
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
  if (!httpRequest("POST", buildRestPath(telemetry_table_), "application/json", payload, status, nullptr)) {
    ESP_LOGW(kTag, "Telemetry publish failed: transport/TLS error");
    return false;
  }
  if (status < 200 || status >= 300) {
    ESP_LOGW(kTag, "Telemetry publish failed: HTTP status=%d", status);
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
    ESP_LOGW(kTag,
             "Status publish failed: transport/TLS error payload=%s",
             logPreview(payload).c_str());
    return false;
  }
  if (status < 200 || status >= 300) {
    ESP_LOGW(kTag,
             "Status publish failed: HTTP status=%d payload=%s",
             status,
             logPreview(payload).c_str());
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
  std::string local_body;
  std::string* body_out = response_body ? response_body : &local_body;
  body_out->clear();

  // Read streaming body in chunks for variable-size JSON responses.
  char buf[256];
  int read = 0;
  do {
    read = esp_http_client_read(client, buf, sizeof(buf));
    if (read > 0) body_out->append(buf, read);
  } while (read > 0);

  if (status_code >= 400) {
    const char* resp_text = body_out->empty() ? "<empty>" : body_out->c_str();
    ESP_LOGW(kTag, "HTTP %s %s returned status=%d body=%s",
             method, url.c_str(), status_code, resp_text);
  }

  esp_http_client_cleanup(client);
  xSemaphoreGive(http_lock_);
  return true;
}

bool NetworkHal::pollCommand() {
  if (!command_handler_) return false;

  int status = 0;
  std::string response;
  const std::string query = buildRestPath(commands_table_) +
                            "?device_id=eq." + device_id_ +
                            "&processed=eq.false&order=id.asc&limit=1&select=id,command";

  if (!httpRequest("GET", query, nullptr, "", status, &response)) {
    ESP_LOGW(kTag, "Command poll failed: transport/TLS error");
    return false;
  }
  if (status < 200 || status >= 300) {
    ESP_LOGW(kTag, "Command poll failed: HTTP status=%d", status);
    return false;
  }

  // Expected response: JSON array with zero or one command row.
  // Empty body is interpreted as "no command available".
  if (response.empty()) {
    // Some backends/proxies may return 204/empty for "no rows"; treat as no command.
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
    return true;
  }

  cJSON* row = cJSON_GetArrayItem(root, 0);
  cJSON* id = cJSON_GetObjectItem(row, "id");
  cJSON* cmd = cJSON_GetObjectItem(row, "command");
  if (!cJSON_IsNumber(id) || !cmd) {
    cJSON_Delete(root);
    ESP_LOGW(kTag, "Command poll failed: malformed command row");
    return false;
  }

  char* cmd_text = cJSON_PrintUnformatted(cmd);
  if (!cmd_text) {
    cJSON_Delete(root);
    ESP_LOGW(kTag, "Command poll failed: JSON serialization error");
    return false;
  }
  command_handler_(std::string(cmd_text));
  cJSON_free(cmd_text);
  const uint64_t cmd_id = static_cast<uint64_t>(id->valuedouble);
  cJSON_Delete(root);
  return markCommandProcessed(cmd_id);
}

bool NetworkHal::markCommandProcessed(uint64_t command_id) {
  int status = 0;
  const std::string query = buildRestPath(commands_table_) + "?id=eq." + std::to_string(command_id);
  if (!httpRequest("PATCH", query, "application/json", "{\"processed\":true}", status, nullptr)) {
    ESP_LOGW(kTag, "Mark command processed failed for id=%llu: transport/TLS error",
             static_cast<unsigned long long>(command_id));
    return false;
  }
  if (status < 200 || status >= 300) {
    ESP_LOGW(kTag, "Mark command processed failed for id=%llu: HTTP status=%d",
             static_cast<unsigned long long>(command_id), status);
    return false;
  }
  return true;
}

std::string NetworkHal::buildRestPath(const char* table) const {
  return std::string("/rest/v1/") + table;
}

std::string NetworkHal::fullUrl(const std::string& path_or_query) const {
  if (!path_or_query.empty() && path_or_query.front() == '/') return std::string(supabase_url_) + path_or_query;
  return std::string(supabase_url_) + "/" + path_or_query;
}

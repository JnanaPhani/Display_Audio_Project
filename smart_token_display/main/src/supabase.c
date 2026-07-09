#include "supabase.h"
#include "esp_crt_bundle.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_websocket_client.h"
#include "esp_wifi.h"
#include "main.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "ota.h"
#include "token_display.h"
#include "wifi_provisioning/manager.h"
#include <time.h>

#define TAG_DB "Supabase"

/* Human-readable reboot reason, set in main.c at boot. */
extern const char *G_REBOOT_REASON_STR;
/* Persisted brownout reboot count, set in main.c at boot. */
extern uint32_t G_BROWNOUT_COUNT;

static esp_websocket_client_handle_t ws_client = NULL;
static int s_consecutive_heartbeat_failures = 0;

struct response_buffer {
  char *buf;
  int idx;
  int size;
};

static esp_err_t _http_event_handler(esp_http_client_event_t *evt) {
  switch (evt->event_id) {
    case HTTP_EVENT_ON_DATA:
      if (evt->user_data) {
        struct response_buffer *res = (struct response_buffer *)evt->user_data;
        if (res->idx + evt->data_len < res->size - 1) {
          memcpy(res->buf + res->idx, evt->data, evt->data_len);
          res->idx += evt->data_len;
          res->buf[res->idx] = '\0';
        }
      }
      break;
    default:
      break;
  }
  return ESP_OK;
}

void check_for_updates_via_http(void);

// Forward Declarations
static void websocket_event_handler(void *handler_args, esp_event_base_t base,
                                    int32_t event_id, void *event_data);
static void set_supabase_auth_headers(esp_http_client_handle_t client);

// Task wrapper to run reprovision with sufficient stack
static void reprovision_task(void *pvParameters) {
  trigger_software_reprovision();
  vTaskDelete(NULL);
}

/**
 * @brief Report whether the device currently has Wi-Fi + an IP address.
 *
 * Used by the offline-first scan path: when this returns false, scans are
 * queued in NVS instead of being POSTed, and flushed once we are back online.
 */
bool supabase_is_online(void) {
  wifi_ap_record_t ap;
  if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) {
    return false; // not associated to an AP
  }
  esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
  if (netif == NULL || !esp_netif_is_netif_up(netif)) {
    return false; // interface down
  }
  esp_netif_ip_info_t ip;
  if (esp_netif_get_ip_info(netif, &ip) != ESP_OK || ip.ip.addr == 0) {
    return false; // no IP yet
  }
  return true;
}

/**
 * Helper: Get MAC Address
 */
void get_mac_address(char *mac_str) {
  uint8_t mac[6];
  esp_efuse_mac_get_default(mac);
  /* 17 chars + NUL; all callers pass char[18]. Bounded to avoid overflow if a
   * caller ever passes a smaller buffer. */
  snprintf(mac_str, 18, "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2],
           mac[3], mac[4], mac[5]);
}

/**
 * Helper: Set standard Headers safely using stack buffer
 */
static void set_supabase_auth_headers(esp_http_client_handle_t client) {
  esp_http_client_set_header(client, "apikey", SUPABASE_ANON_KEY);
  // Use a stack buffer large enough for "Bearer " + key
  char bearer[1024];
  snprintf(bearer, sizeof(bearer), "Bearer %s", SUPABASE_ANON_KEY);
  // esp_http_client_set_header copies the value internally, so stack is safe
  esp_http_client_set_header(client, "Authorization", bearer);
  esp_http_client_set_header(client, "Content-Type", "application/json");
}

/**
 * @brief Handle a device_status realtime record (reprovision trigger).
 */
static void handle_device_status_record(cJSON *data_node) {
  if (!data_node)
    return;
  cJSON *dev_id = cJSON_GetObjectItem(data_node, "device_id");
  if (!dev_id || !cJSON_IsString(dev_id))
    return;

  char my_mac[18];
  get_mac_address(my_mac);
  if (strcasecmp(dev_id->valuestring, my_mac) != 0) {
    return; // Not this device
  }

  // 1. Check reprovision trigger
  cJSON *trig = cJSON_GetObjectItem(data_node, "reprovision_trigger");
  if (trig && cJSON_IsNumber(trig) && trig->valueint == 0) {
    ESP_LOGE(TAG_DB, "SOFTWARE REPROVISION TRIGGERED!");
    xTaskCreate(reprovision_task, "reprov_task", 8192, NULL, 5, NULL);
  }

  // 2. Check brightness adjustment
  cJSON *bright = cJSON_GetObjectItem(data_node, "brightness");
  if (bright && cJSON_IsNumber(bright)) {
    int val = bright->valueint;
    if (val >= 1 && val <= 100) {
      nvs_handle_t handle;
      uint32_t saved_brightness = 75;
      bool is_different = true;
      if (nvs_open("storage", NVS_READONLY, &handle) == ESP_OK) {
        if (nvs_get_u32(handle, "brightness", &saved_brightness) == ESP_OK) {
          if (saved_brightness == val) {
            is_different = false;
          }
        }
        nvs_close(handle);
      }
      if (is_different) {
        ESP_LOGI(TAG_DB, "Realtime update: setting brightness to %d%%", val);
        token_display_set_brightness((uint8_t)val);
      }
    } else {
      ESP_LOGW(TAG_DB, "Realtime update: invalid brightness percent %d", val);
    }
  }

  // 3. Check name adjustment
  cJSON *name_item = cJSON_GetObjectItem(data_node, "name");
  if (name_item) {
    if (cJSON_IsString(name_item)) {
      nvs_handle_t handle;
      char saved_name[64] = "";
      size_t required_size = sizeof(saved_name);
      bool is_different = true;
      if (nvs_open("storage", NVS_READONLY, &handle) == ESP_OK) {
        if (nvs_get_str(handle, "device_name", saved_name, &required_size) == ESP_OK) {
          if (strcmp(saved_name, name_item->valuestring) == 0) {
            is_different = false;
          }
        }
        nvs_close(handle);
      }
      if (is_different) {
        ESP_LOGI(TAG_DB, "Realtime update: saving name '%s' to NVS", name_item->valuestring);
        if (nvs_open("storage", NVS_READWRITE, &handle) == ESP_OK) {
          nvs_set_str(handle, "device_name", name_item->valuestring);
          nvs_commit(handle);
          nvs_close(handle);
        }
      }
    } else if (cJSON_IsNull(name_item)) {
      nvs_handle_t handle;
      if (nvs_open("storage", NVS_READWRITE, &handle) == ESP_OK) {
        nvs_erase_key(handle, "device_name");
        nvs_commit(handle);
        nvs_close(handle);
      }
    }
  }
}

/**
 * @brief Extract the postgres record (new row) from a realtime payload.
 *
 * Supports both the postgres_changes format (payload.data.record) and the
 * legacy formats (payload.record / payload.new).
 */
static cJSON *extract_record(cJSON *payload) {
  if (!payload)
    return NULL;
  cJSON *data_wrapper = cJSON_GetObjectItem(payload, "data");
  cJSON *rec = NULL;
  if (data_wrapper) {
    rec = cJSON_GetObjectItem(data_wrapper, "record");
    if (!rec)
      rec = cJSON_GetObjectItem(data_wrapper, "new");
  }
  if (!rec)
    rec = cJSON_GetObjectItem(payload, "record");
  if (!rec)
    rec = cJSON_GetObjectItem(payload, "new");
  return rec;
}

/**
 * WebSocket Event Handler
 */
static void websocket_event_handler(void *handler_args, esp_event_base_t base,
                                    int32_t event_id, void *event_data) {
  esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;

  switch (event_id) {
  case WEBSOCKET_EVENT_CONNECTED:
    ESP_LOGI(TAG_DB, "WebSocket Connected. Joining Topics...");

#if ENABLE_OTA
    const char *js =
        "{\"topic\":\"realtime:public:system_control\",\"event\":\"phx_join\","
        "\"payload\":{\"config\":{\"postgres_changes\":[{\"event\":\"*\","
        "\"schema\":\"public\",\"table\":\"system_control\"}]}},\"ref\":\"1\"}";
    esp_websocket_client_send_text(ws_client, js, strlen(js), portMAX_DELAY);
#endif

    const char *jd =
        "{\"topic\":\"realtime:public:device_status\",\"event\":\"phx_join\","
        "\"payload\":{\"config\":{\"postgres_changes\":[{\"event\":\"UPDATE\","
        "\"schema\":\"public\",\"table\":\"device_status\"}]}},\"ref\":\"2\"}";
    esp_websocket_client_send_text(ws_client, jd, strlen(jd), portMAX_DELAY);
    break;

  case WEBSOCKET_EVENT_DATA: {
    // Skip fragmented messages - only process when we have the complete payload
    if (data->payload_offset != 0 || data->data_len != data->payload_len) {
      ESP_LOGW(
          TAG_DB,
          "WS fragmented msg: offset=%d, data_len=%d, payload_len=%d (skipped)",
          data->payload_offset, data->data_len, data->payload_len);
      break;
    }
    if (data->data_ptr == NULL || data->data_len <= 0) {
      break;
    }

    cJSON *root = cJSON_ParseWithLength(data->data_ptr, data->data_len);
    if (!root)
      break;

    cJSON *topic = cJSON_GetObjectItem(root, "topic");
    cJSON *payload = cJSON_GetObjectItem(root, "payload");
    const char *topic_str = cJSON_IsString(topic) ? topic->valuestring : "";

    if (strcmp(topic_str, "realtime:public:system_control") == 0) {
      ota_handle_system_control_record(extract_record(payload));
    } else if (strcmp(topic_str, "realtime:public:device_status") == 0) {
      handle_device_status_record(extract_record(payload));
    }

    cJSON_Delete(root);
    break;
  }

  case WEBSOCKET_EVENT_DISCONNECTED:
    ESP_LOGE(TAG_DB, "WebSocket Disconnected");
    break;
  }
}

void start_supabase_realtime(void) {
  char ws_url[1280];
  // Build the realtime URL from SUPABASE_URL (https -> wss).
  const char *host = SUPABASE_URL;
  if (strncmp(host, "https://", 8) == 0) {
    host += 8;
  }
  snprintf(ws_url, sizeof(ws_url),
           "wss://%s/realtime/v1/websocket?apikey=%s&vsn=1.0.0", host,
           SUPABASE_ANON_KEY);

  esp_websocket_client_config_t ws_cfg = {
      .uri = ws_url,
      .buffer_size = 4096,
      .crt_bundle_attach = esp_crt_bundle_attach,
  };
  ws_client = esp_websocket_client_init(&ws_cfg);
  esp_websocket_register_events(ws_client, WEBSOCKET_EVENT_ANY,
                                websocket_event_handler, NULL);
  esp_websocket_client_start(ws_client);
}

/**
 * @brief Record a token lifecycle event via the record_token_event RPC.
 *
 * Sends the decoded barcode fields and a status to Supabase. The RPC appends to
 * the token_events history table and upserts the latest per-order status.
 *
 *   order_type : "online" | "offline"
 *   status     : "packing_complete" | "ready_to_collect" | "no_show"
 *   event_date : "YYYY-MM-DD" decoded from the barcode (may be NULL)
 *
 * @return ESP_OK on success, ESP_FAIL on any network/HTTP failure (caller
 * should then queue the event for later retry).
 */
esp_err_t supabase_post_rpc(const char *rpc_name, const char *json_body) {
  if (!rpc_name || !json_body)
    return ESP_FAIL;

  char url[256];
  snprintf(url, sizeof(url), "%s/rest/v1/rpc/%s", SUPABASE_URL, rpc_name);
  esp_http_client_config_t config = {
      .url = url,
      .method = HTTP_METHOD_POST,
      .timeout_ms = 10000,
      .buffer_size = SB_HTTP_BUFFER_SIZE,
      .buffer_size_tx = SB_HTTP_TX_BUFFER_SIZE,
      .crt_bundle_attach = esp_crt_bundle_attach,
  };
  esp_http_client_handle_t client = esp_http_client_init(&config);
  set_supabase_auth_headers(client);
  esp_http_client_set_header(client, "Prefer", "return=minimal");
  esp_http_client_set_post_field(client, json_body, strlen(json_body));

  esp_err_t result = ESP_FAIL;
  esp_err_t err = esp_http_client_perform(client);
  if (err == ESP_OK) {
    int code = esp_http_client_get_status_code(client);
    if (code >= 200 && code < 300) {
      result = ESP_OK;
    } else {
      ESP_LOGE(TAG_DB, "rpc %s HTTP %d", rpc_name, code);
    }
  } else {
    ESP_LOGE(TAG_DB, "rpc %s failed: %s", rpc_name, esp_err_to_name(err));
  }
  esp_http_client_cleanup(client);
  return result;
}

esp_err_t post_token_event(const char *order_id, const char *token,
                           const char *order_type, const char *status,
                           const char *event_date) {
  if (!order_id || !token || !order_type || !status)
    return ESP_FAIL;

  char mac_str[18];
  get_mac_address(mac_str);

  cJSON *root = cJSON_CreateObject();
  cJSON_AddStringToObject(root, "p_order_id", order_id);
  cJSON_AddStringToObject(root, "p_token", token);
  cJSON_AddStringToObject(root, "p_order_type", order_type);
  cJSON_AddStringToObject(root, "p_status", status);
  if (event_date && event_date[0]) {
    cJSON_AddStringToObject(root, "p_event_date", event_date);
  } else {
    cJSON_AddNullToObject(root, "p_event_date");
  }
  cJSON_AddStringToObject(root, "p_device_id", mac_str);
  char *json_string = cJSON_PrintUnformatted(root);

  char url[256];
  snprintf(url, sizeof(url), "%s/rest/v1/rpc/record_token_event", SUPABASE_URL);
  esp_http_client_config_t config = {
      .url = url,
      .method = HTTP_METHOD_POST,
      .timeout_ms = 10000,
      .buffer_size = SB_HTTP_BUFFER_SIZE,
      .buffer_size_tx = SB_HTTP_TX_BUFFER_SIZE,
      .crt_bundle_attach = esp_crt_bundle_attach,
  };
  esp_http_client_handle_t client = esp_http_client_init(&config);
  set_supabase_auth_headers(client);
  esp_http_client_set_header(client, "Prefer", "return=minimal");
  esp_http_client_set_post_field(client, json_string, strlen(json_string));

  esp_err_t result = ESP_FAIL;
  esp_err_t err = esp_http_client_perform(client);
  if (err == ESP_OK) {
    int code = esp_http_client_get_status_code(client);
    if (code >= 200 && code < 300) {
      result = ESP_OK;
      ESP_LOGI(TAG_DB, "token_event ok: %s token=%s type=%s status=%s",
               order_id, token, order_type, status);
    } else {
      ESP_LOGE(TAG_DB, "token_event HTTP %d for %s/%s", code, order_id, status);
    }
  } else {
    ESP_LOGE(TAG_DB, "token_event POST failed: %s", esp_err_to_name(err));
  }
  esp_http_client_cleanup(client);
  cJSON_Delete(root);
  free(json_string);
  return result;
}

void send_device_health_to_supabase(void) {
  char mac_str[18];
  get_mac_address(mac_str);

  // SSID (not password) for monitoring
  wifi_config_t wifi_cfg;
  char current_ssid[33] = {0};
  if (esp_wifi_get_config(WIFI_IF_STA, &wifi_cfg) == ESP_OK) {
    strncpy(current_ssid, (char *)wifi_cfg.sta.ssid, sizeof(current_ssid) - 1);
  }

  // Provisioned timestamp from NVS (if any)
  int64_t last_prov_ts = 0;
  char saved_name[64] = "";
  size_t required_size = sizeof(saved_name);
  nvs_handle_t handle;
  if (nvs_open("storage", NVS_READONLY, &handle) == ESP_OK) {
    nvs_get_i64(handle, "prov_ts", &last_prov_ts);
    nvs_get_str(handle, "device_name", saved_name, &required_size);
    nvs_close(handle);
  }

  cJSON *root = cJSON_CreateObject();
  cJSON_AddStringToObject(root, "device_id", mac_str);
  cJSON_AddStringToObject(root, "wifi_ssid", current_ssid);
  cJSON_AddStringToObject(root, "device_type", DEVICE_TYPE);
  if (saved_name[0]) {
    cJSON_AddStringToObject(root, "name", saved_name);
  } else {
    cJSON_AddNullToObject(root, "name");
  }

  wifi_ap_record_t ap;
  if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
    cJSON_AddNumberToObject(root, "wifi_rssi", ap.rssi);
  }

  cJSON_AddNumberToObject(root, "uptime_seconds",
                          esp_timer_get_time() / 1000000);
  cJSON_AddStringToObject(root, "firmware_version", FIRMWARE_VERSION);
  cJSON_AddStringToObject(root, "last_reboot_reason",
                          G_REBOOT_REASON_STR ? G_REBOOT_REASON_STR
                                              : "Unknown");
  cJSON_AddNumberToObject(root, "brownout_count", (double)G_BROWNOUT_COUNT);

  time_t now;
  time(&now);
  cJSON_AddNumberToObject(root, "device_timestamp", (double)now);

  if (last_prov_ts > 0) {
    char prov_time_str[32];
    struct tm timeinfo;
    time_t t = (time_t)last_prov_ts;
    gmtime_r(&t, &timeinfo);
    strftime(prov_time_str, sizeof(prov_time_str), "%Y-%m-%dT%H:%M:%SZ",
             &timeinfo);
    cJSON_AddStringToObject(root, "last_provisioned_at", prov_time_str);
  } else {
    cJSON_AddNullToObject(root, "last_provisioned_at");
  }

  char *json_string = cJSON_PrintUnformatted(root);

  char url[256];
  snprintf(url, sizeof(url), "%s/rest/v1/%s", SUPABASE_URL,
           TABLE_DEVICE_STATUS);
  esp_http_client_config_t config = {
      .url = url,
      .method = HTTP_METHOD_POST,
      .timeout_ms = 10000,
      .buffer_size = SB_HTTP_BUFFER_SIZE,
      .buffer_size_tx = SB_HTTP_TX_BUFFER_SIZE,
      .crt_bundle_attach = esp_crt_bundle_attach,
  };
  esp_http_client_handle_t client = esp_http_client_init(&config);
  set_supabase_auth_headers(client);
  esp_http_client_set_header(client, "Prefer", "resolution=merge-duplicates");
  esp_http_client_set_post_field(client, json_string, strlen(json_string));

  ESP_LOGI(TAG_DB, "Sending device health heartbeat: %s", json_string);

  esp_err_t err = esp_http_client_perform(client);
  if (err == ESP_OK) {
    int code = esp_http_client_get_status_code(client);
    if (code >= 200 && code < 300) {
      ESP_LOGI(TAG_DB, "Heartbeat sent successfully (HTTP %d, SSID: %s)", code,
               current_ssid);
      s_consecutive_heartbeat_failures = 0;
    } else {
      ESP_LOGE(TAG_DB, "Heartbeat failed with HTTP %d", code);
      s_consecutive_heartbeat_failures++;
    }
  } else {
    ESP_LOGE(TAG_DB, "Heartbeat transport failed: %s", esp_err_to_name(err));
    s_consecutive_heartbeat_failures++;
  }
  esp_http_client_cleanup(client);
  cJSON_Delete(root);
  free(json_string);

  if (s_consecutive_heartbeat_failures >= 5) {
    ESP_LOGW(TAG_DB, "Heartbeat failed 5 consecutive times while online. Reconnecting Wi-Fi to recover DNS/sockets...");
    s_consecutive_heartbeat_failures = 0;
    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_wifi_connect();
  }
}

void check_for_updates_via_http(void) {
  char url[256];
  snprintf(url, sizeof(url), "%s/rest/v1/system_control?id=eq.1", SUPABASE_URL);

  struct response_buffer res_buf;
  char buffer[2048];
  res_buf.buf = buffer;
  res_buf.idx = 0;
  res_buf.size = sizeof(buffer);
  memset(buffer, 0, sizeof(buffer));

  esp_http_client_config_t config = {
      .url = url,
      .method = HTTP_METHOD_GET,
      .timeout_ms = 10000,
      .event_handler = _http_event_handler,
      .user_data = &res_buf,
      .crt_bundle_attach = esp_crt_bundle_attach,
  };
  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (!client) {
    ESP_LOGE(TAG_DB, "Failed to initialize HTTP client for update check");
    return;
  }
  set_supabase_auth_headers(client);

  esp_err_t err = esp_http_client_perform(client);
  if (err == ESP_OK) {
    int status = esp_http_client_get_status_code(client);
    if (status >= 200 && status < 300) {
      cJSON *root = cJSON_Parse(buffer);
      if (root && cJSON_IsArray(root)) {
        cJSON *row = cJSON_GetArrayItem(root, 0);
        if (row) {
          ESP_LOGI(TAG_DB, "HTTP update check: got system_control row");
          ota_handle_system_control_record(row);
        }
      } else {
        ESP_LOGE(TAG_DB, "HTTP update check: invalid response JSON or empty array");
      }
      if (root) {
        cJSON_Delete(root);
      }
    } else {
      ESP_LOGE(TAG_DB, "HTTP update check failed with HTTP status %d", status);
    }
  } else {
    ESP_LOGE(TAG_DB, "HTTP update check perform failed: %s", esp_err_to_name(err));
  }
  esp_http_client_cleanup(client);
}

void supabase_sync_task(void *pvParameters) {
  // Wait until Wi-Fi is connected
  while (esp_wifi_sta_get_ap_info(&(wifi_ap_record_t){0}) != ESP_OK) {
    vTaskDelay(pdMS_TO_TICKS(1000));
  }

  start_supabase_realtime();

  uint32_t last_health = 0, last_heartbeat = 0;
  while (1) {
    uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;

    // Phoenix websocket keepalive every 30s
    if (now - last_heartbeat >= 30000) {
      if (ws_client && esp_websocket_client_is_connected(ws_client)) {
        const char *hb = "{\"topic\":\"phoenix\",\"event\":\"heartbeat\","
                         "\"payload\":{},\"ref\":\"hb\"}";
        ESP_LOGI(TAG_DB, "Sending Phoenix keepalive: %s", hb);
        esp_websocket_client_send_text(ws_client, hb, strlen(hb),
                                       portMAX_DELAY);
      }
      last_heartbeat = now;
    }

    // Device health heartbeat every 60s
    if (last_health == 0 || now - last_health >= 60000) {
      if (supabase_is_online()) {
        send_device_health_to_supabase();
        check_for_updates_via_http();
      }
      last_health = now;
    }

    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}

void trigger_software_reprovision(void) {
  // Final heartbeat to acknowledge/reset the trigger
  char mac[18];
  get_mac_address(mac);
  cJSON *root = cJSON_CreateObject();
  cJSON_AddStringToObject(root, "device_id", mac);
  cJSON_AddNumberToObject(root, "reprovision_trigger", 1);
  cJSON_AddStringToObject(root, "device_type", DEVICE_TYPE);
  char *js = cJSON_PrintUnformatted(root);

  char url[256];
  snprintf(url, sizeof(url), "%s/rest/v1/%s", SUPABASE_URL,
           TABLE_DEVICE_STATUS);
  esp_http_client_config_t cfg = {
      .url = url,
      .method = HTTP_METHOD_POST,
      .buffer_size = SB_HTTP_BUFFER_SIZE,
      .buffer_size_tx = SB_HTTP_TX_BUFFER_SIZE,
      .crt_bundle_attach = esp_crt_bundle_attach,
  };
  esp_http_client_handle_t cli = esp_http_client_init(&cfg);
  set_supabase_auth_headers(cli);
  esp_http_client_set_header(cli, "Prefer", "resolution=merge-duplicates");
  esp_http_client_set_post_field(cli, js, strlen(js));
  esp_http_client_perform(cli);
  esp_http_client_cleanup(cli);
  cJSON_Delete(root);
  free(js);

  ESP_LOGW(TAG_DB, "Erasing WiFi and restarting...");
  wifi_prov_mgr_reset_provisioning();
  vTaskDelay(pdMS_TO_TICKS(2000));
  esp_restart();
}

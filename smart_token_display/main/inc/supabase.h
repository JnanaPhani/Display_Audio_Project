#ifndef __SUPABASE_H__
#define __SUPABASE_H__

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "cJSON.h"

#include "lwip/err.h"
#include "lwip/sys.h"

#include "esp_http_client.h"
#include "esp_mac.h" 
#include "esp_app_desc.h"
#include "nvs_flash.h"
#include "nvs.h"

#include "provisioning.h"
#include "wifi.h"
/* ==================================================================== */
/* ==================== CONFIGURATION ================================= */
/* ==================================================================== */
#define SB_HTTP_BUFFER_SIZE    4096
#define SB_HTTP_TX_BUFFER_SIZE 4096

// --- Supabase Configuration ---
// Injected at build time from the project-root .env via load_env.cmake
// (CFG_SUPABASE_URL / CFG_SUPABASE_ANON_KEY). Do NOT hardcode secrets here.
// The fallbacks below only exist so the file still compiles if .env is missing;
// a real build with .env present overrides them.
#ifdef CFG_SUPABASE_URL
#define SUPABASE_URL CFG_SUPABASE_URL
#else
#define SUPABASE_URL ""
#endif

#ifdef CFG_SUPABASE_ANON_KEY
#define SUPABASE_ANON_KEY CFG_SUPABASE_ANON_KEY
#else
#define SUPABASE_ANON_KEY ""
#endif

// Table Names

#define TABLE_DEVICE_STATUS "device_status"

#define TABLE_SYSTEM_CONTROL "system_control"

#define TABLE_TOKEN_SCANS "token_scans"

/*
 * Firmware version is auto-derived at build time by ESP-IDF from `git describe`
 * (project root). Tag releases with e.g. `git tag v1.0.1` so this reflects the
 * release. Resolved at runtime from the app descriptor, so it is a const char*
 * (not a string literal). Used for the boot log, heartbeat, and the OTA
 * version comparison against system_control.version.
 *
 * NOTE: OTA compares this string for equality. Use clean release tags (v1.0.1)
 * rather than relying on the "-<n>-g<hash>" dirty/commit suffix, otherwise every
 * commit changes the version and may re-trigger OTA.
 */
#define FIRMWARE_VERSION (esp_app_get_description()->version)

/* ==================== FUNCTIONS ===================================== */

/**
 * @brief Sends heartbeats to the 'device_status' table.
 * Includes RSSI and uptime for monitoring.
 */
void send_device_health_to_supabase(void);

/**
 * @brief Background task to periodically sync status or check for remote commands.
 */
void supabase_sync_task(void *pvParameters);

// New function for Realtime
void start_supabase_realtime(void);

void trigger_software_reprovision(void);
void get_mac_address(char *mac_str);

/**
 * @brief Report whether the device currently has Wi-Fi + an IP address.
 *
 * @return true if associated and an IP is assigned, false otherwise.
 */
bool supabase_is_online(void);

/**
 * @brief Record a token lifecycle event via the record_token_event RPC.
 *
 * @param order_id   Raw scanned barcode (e.g. "BY-20260615-T42").
 * @param token      Decoded token ("42" or "ORD42").
 * @param order_type "online" or "offline".
 * @param status     "packing_complete" | "ready_to_collect" | "no_show".
 * @param event_date "YYYY-MM-DD" from the barcode, or NULL.
 * @return ESP_OK on success, ESP_FAIL on failure (caller should queue + retry).
 */
esp_err_t post_token_event(const char *order_id, const char *token,
                           const char *order_type, const char *status,
                           const char *event_date);

/**
 * @brief POST a JSON body to a Supabase PostgREST RPC (/rest/v1/rpc/<name>).
 *
 * Shared helper used by the crash + OTA status reporters. Sends with the
 * standard apikey/Bearer headers and Prefer: return=minimal.
 *
 * @param rpc_name  RPC function name (e.g. "report_crash").
 * @param json_body Pre-serialized JSON object string.
 * @return ESP_OK on 2xx, ESP_FAIL otherwise.
 */
esp_err_t supabase_post_rpc(const char *rpc_name, const char *json_body);

#endif // __SUPABASE_H__
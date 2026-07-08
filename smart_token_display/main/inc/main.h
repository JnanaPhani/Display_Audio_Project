#ifndef __MAIN_H__
#define __MAIN_H__

#include <stdio.h>
#include <inttypes.h>
#include <string.h>
#include <sys/time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_wifi.h"
#include "esp_sntp.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_app_format.h"
#include "nvs_flash.h"

/* ==================================================================== */
/* ==================== FEATURE TOGGLES =============================== */
/* ==================================================================== */

/**
 * @brief Enable/disable Over-The-Air firmware updates.
 *   1 = OTA enabled  (system_control realtime channel + esp_https_ota).
 *   0 = OTA disabled (OTA code compiled out as no-op stubs; the
 *       system_control websocket topic is not joined).
 */
#define ENABLE_OTA 1

/**
 * @brief Enable/disable ESP-NOW broadcast of scan events to the gateway ESP.
 *   1 = ESP-NOW enabled  (each accepted scan is also unicast to GATEWAY_MAC).
 *   0 = ESP-NOW disabled (espnow code compiled out as no-op stubs).
 *
 * Independent of Wi-Fi/Supabase: ESP-NOW keeps working even when the shop
 * router is off, as long as all devices share ESPNOW_CHANNEL (see espnow.h).
 */
#define ENABLE_ESPNOW 1

void print_system_memory_status();
void initialize_sntp(void);
void handle_provisioning_timestamp();
const char* get_reboot_reason_string(esp_reset_reason_t reason);

#endif /* __MAIN_H__ */

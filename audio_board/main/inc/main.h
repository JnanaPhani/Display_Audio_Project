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
 *   0 = OTA disabled.
 */
#define ENABLE_OTA 1

/**
 * @brief Device type string used in Supabase logs and OTA updates.
 */
#define DEVICE_TYPE "speaker"

/**
 * @brief Enable/disable ESP-NOW broadcast of scan events to the gateway ESP.
 *   1 = ESP-NOW enabled.
 *   0 = ESP-NOW disabled.
 */
#define ENABLE_ESPNOW 1

/* Project components */
#include "crash_report.h"
#include "espnow.h"
#include "mp3board.h"
#include "ota.h"
#include "ota_selftest.h"
#include "provisioning.h"
#include "supabase.h"
#include "wifi.h"

void print_system_memory_status();
void initialize_sntp(void);
void handle_provisioning_timestamp();
const char* get_reboot_reason_string(esp_reset_reason_t reason);

#endif /* __MAIN_H__ */

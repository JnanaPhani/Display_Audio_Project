#ifndef __OTA_H__
#define __OTA_H__

#include "cJSON.h"

/**
 * @brief OTA (Over-The-Air firmware update) module.
 *
 * Compiled and active only when ENABLE_OTA (see main.h) is set to 1. When
 * ENABLE_OTA is 0, all public functions become no-ops so the rest of the
 * firmware can call them unconditionally.
 */

/**
 * @brief Start an OTA update from the given HTTPS firmware URL.
 *
 * Spawns a background task that downloads and applies the image, then reboots
 * on success. No-op when ENABLE_OTA == 0 or url is NULL.
 *
 * @param url HTTPS URL to the firmware .bin.
 */
void start_ota_update(const char *url);

/**
 * @brief Background OTA task entry point (downloads + applies + reboots).
 *
 * @param pvParameter Heap-allocated char* URL; freed by the task.
 */
void ota_task(void *pvParameter);

/**
 * @brief Evaluate a Supabase 'system_control' record and trigger OTA if the
 *        advertised version differs from the running FIRMWARE_VERSION.
 *
 * No-op when ENABLE_OTA == 0. Safe to call with NULL.
 *
 * @param record cJSON object with "version" and "bin_url" string fields.
 */
void ota_handle_system_control_record(cJSON *record);

#endif /* __OTA_H__ */

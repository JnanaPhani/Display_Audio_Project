#include "ota_selftest.h"
#include "supabase.h"
#include "main.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "cJSON.h"

#define TAG_ST "OTASelfTest"

/* Hard-gate timeout: Wi-Fi must get an IP within this window after boot. */
#define SELFTEST_WIFI_TIMEOUT_MS   60000
/* Soft-gate grace: how long to keep trying Supabase before reporting degraded. */
#define SELFTEST_CLOUD_GRACE_MS    300000   /* 5 min */
#define SELFTEST_CLOUD_RETRY_MS    15000

extern const char *G_REBOOT_REASON_STR;  /* unused here but kept for symmetry */

/* Is this boot an unverified (just-OTA'd) image awaiting validation? */
static bool s_pending_verify = false;
static volatile bool s_peripherals_ok = false;

void ota_selftest_set_peripherals_ok(bool ok) {
    s_peripherals_ok = ok;
}

void ota_selftest_init(void) {
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(running, &state) != ESP_OK) {
        return;
    }
    if (state == ESP_OTA_IMG_PENDING_VERIFY) {
        s_pending_verify = true;
        ESP_LOGW(TAG_ST, "Pending-verify OTA image: self-test required or rollback");
    }
}

/** @brief Report an ota_status phase for this device. @return ESP_OK on 2xx. */
static esp_err_t report_phase(const char *phase) {
    char mac_str[18];
    get_mac_address(mac_str);
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "p_device_id", mac_str);
    cJSON_AddNullToObject(root, "p_from_version");
    cJSON_AddStringToObject(root, "p_to_version", FIRMWARE_VERSION);
    cJSON_AddStringToObject(root, "p_phase", phase);
    cJSON_AddNullToObject(root, "p_error");
    cJSON_AddNullToObject(root, "p_attempt");
    char *body = cJSON_PrintUnformatted(root);
    esp_err_t r = supabase_post_rpc("report_ota_status", body);
    cJSON_Delete(root);
    free(body);
    return r;
}

static void rollback_now(const char *why) {
    ESP_LOGE(TAG_ST, "SELF-TEST FAILED (%s) -> rolling back to previous firmware", why);
    /* Marks the running app invalid and reboots into the previous valid image. */
    esp_ota_mark_app_invalid_rollback_and_reboot();
    /* Does not return. */
    vTaskDelay(portMAX_DELAY);
}

void ota_selftest_run_network_checks(void) {
    if (!s_pending_verify) {
        return;  /* already-validated normal boot */
    }

    /* Tier 1 (HARD): peripherals initialized? token_display_start() sets this. */
    if (!s_peripherals_ok) {
        rollback_now("peripherals init failed");
    }

    /* Tier 2 (HARD): Wi-Fi got an IP within the timeout?
     * This function is called from the network task AFTER connect_wifi()
     * returns, which already blocks until an IP is acquired. So reaching here
     * means Tier 2 passed. Guard anyway with supabase_is_online(). */
    uint32_t waited = 0;
    while (!supabase_is_online() && waited < SELFTEST_WIFI_TIMEOUT_MS) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        waited += 1000;
    }
    if (!supabase_is_online()) {
        rollback_now("no Wi-Fi/IP within timeout");
    }

    /* Both hard gates passed: the image is NOT broken/crash-looping. Mark it
     * valid now so a subsequent cloud outage can't roll back a good build. */
    esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    if (err == ESP_OK) {
        ESP_LOGW(TAG_ST, "Hard gates passed -> firmware marked VALID");
    } else {
        ESP_LOGE(TAG_ST, "mark_app_valid failed: %s", esp_err_to_name(err));
    }
    s_pending_verify = false;

    /* Tier 3 (SOFT): Supabase reachable? Report success/degraded, never roll back.
     * The report_phase() call itself is the reachability probe — if it returns
     * OK the cloud is reachable (phase=success), otherwise retry within the
     * grace window, and finally record degraded. */
    uint32_t cloud_waited = 0;
    bool cloud_ok = false;
    while (cloud_waited < SELFTEST_CLOUD_GRACE_MS) {
        if (report_phase("success") == ESP_OK) {
            cloud_ok = true;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(SELFTEST_CLOUD_RETRY_MS));
        cloud_waited += SELFTEST_CLOUD_RETRY_MS;
    }
    if (!cloud_ok) {
        report_phase("degraded");  /* best-effort; may also fail if truly offline */
    }
    ESP_LOGW(TAG_ST, "Self-test complete: %s", cloud_ok ? "SUCCESS" : "DEGRADED (cloud unreachable)");
}

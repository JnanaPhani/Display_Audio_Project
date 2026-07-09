#include "ota.h"
#include "main.h"
#include "supabase.h"

#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_ota_ops.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"

#define TAG_OTA "OTA"

#if ENABLE_OTA

#define OTA_MAX_ATTEMPTS 4
static const int OTA_BACKOFF_MS[OTA_MAX_ATTEMPTS] = {0, 5000, 15000, 45000};

/** @brief Report an OTA outcome to Supabase ota_status (best-effort). */
static void ota_report(const char *to_version, const char *phase,
                       const char *error, int attempt) {
    char mac_str[18];
    get_mac_address(mac_str);
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "p_device_id", mac_str);
    cJSON_AddStringToObject(root, "p_from_version", FIRMWARE_VERSION);
    cJSON_AddStringToObject(root, "p_to_version", to_version ? to_version : "");
    cJSON_AddStringToObject(root, "p_phase", phase);
    if (error) cJSON_AddStringToObject(root, "p_error", error);
    else       cJSON_AddNullToObject(root, "p_error");
    if (attempt >= 0) cJSON_AddNumberToObject(root, "p_attempt", attempt);
    else              cJSON_AddNullToObject(root, "p_attempt");
    char *body = cJSON_PrintUnformatted(root);
    supabase_post_rpc("report_ota_status", body);
    cJSON_Delete(root);
    free(body);
}

/* Heap-allocated work item passed to the OTA task. */
typedef struct {
    char url[512];
    char version[48];
} ota_job_t;

/**
 * @brief Background OTA task: download + apply firmware with retry/backoff,
 *        report every outcome, reboot on success (into pending-verify).
 */
void ota_task(void *pvParameter) {
    ota_job_t *job = (ota_job_t *)pvParameter;
    ESP_LOGW(TAG_OTA, "OTA TASK: target=%s url=[%s]", job->version, job->url);

    esp_http_client_config_t config = {
        .url = job->url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .keep_alive_enable = true,
        .timeout_ms = 60000,
        .buffer_size = 10240,
    };
    esp_https_ota_config_t ota_config = { .http_config = &config };

    esp_err_t ret = ESP_FAIL;
    for (int attempt = 1; attempt <= OTA_MAX_ATTEMPTS; attempt++) {
        if (attempt > 1) {
            ESP_LOGW(TAG_OTA, "Retry %d/%d in %d ms", attempt, OTA_MAX_ATTEMPTS,
                     OTA_BACKOFF_MS[attempt - 1]);
            vTaskDelay(pdMS_TO_TICKS(OTA_BACKOFF_MS[attempt - 1]));
        }
        ret = esp_https_ota(&ota_config);
        if (ret == ESP_OK) {
            break;
        }
        ESP_LOGE(TAG_OTA, "OTA attempt %d failed: %s", attempt, esp_err_to_name(ret));
        ota_report(job->version, "failed", esp_err_to_name(ret), attempt);
        /* Image-invalid / no-space won't fix on retry; stop early. */
        if (ret == ESP_ERR_OTA_VALIDATE_FAILED || ret == ESP_ERR_NO_MEM) {
            break;
        }
    }

    if (ret == ESP_OK) {
        ESP_LOGW(TAG_OTA, "OTA applied! Rebooting into pending-verify...");
        ota_report(job->version, "applied", NULL, -1);
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_restart();
    } else {
        ESP_LOGE(TAG_OTA, "OTA gave up after %d attempts", OTA_MAX_ATTEMPTS);
    }
    free(job);
    vTaskDelete(NULL);
}

/**
 * @brief Validate that an OTA URL points at OUR Supabase Storage, over HTTPS.
 *
 * SECURITY: bin_url arrives over the realtime channel and the system_control
 * row is writable with the (flash-embedded, public) anon key. Without this
 * check, anyone who can write that row — or spoof the realtime message — could
 * point every device at a malicious firmware image. We require the URL to be
 * HTTPS and to begin with our project's public firmware-storage prefix derived
 * from the compiled-in SUPABASE_URL. TLS alone is NOT enough: a malicious host
 * also serves valid HTTPS.
 *
 * @return true if the URL is allowed.
 */
static bool ota_url_is_trusted(const char *url) {
    if (!url) return false;
    /* Must be HTTPS (never plain http for firmware). */
    if (strncmp(url, "https://", 8) != 0) {
        ESP_LOGE(TAG_OTA, "Rejected OTA URL (not https): %s", url);
        return false;
    }
    /* Must live under our project's public storage path. SUPABASE_URL is the
     * compiled-in project base (e.g. https://<ref>.supabase.co). */
    char prefix[256];
    int n = snprintf(prefix, sizeof(prefix),
                     "%s/storage/v1/object/public/", SUPABASE_URL);
    if (n <= 0 || n >= (int)sizeof(prefix) || SUPABASE_URL[0] == '\0') {
        ESP_LOGE(TAG_OTA, "OTA URL check unavailable (SUPABASE_URL unset)");
        return false;
    }
    if (strncmp(url, prefix, strlen(prefix)) != 0) {
        ESP_LOGE(TAG_OTA, "Rejected OTA URL (untrusted host/path): %s", url);
        return false;
    }
    return true;
}

/** @brief Spawn the OTA task for a (url, version) target. */
static void ota_spawn(const char *url, const char *version) {
    if (!ota_url_is_trusted(url)) {
        ota_report(version, "failed", "untrusted_url", -1);
        return;
    }
    ota_job_t *job = calloc(1, sizeof(ota_job_t));
    if (!job) return;
    strncpy(job->url, url, sizeof(job->url) - 1);
    strncpy(job->version, version ? version : "", sizeof(job->version) - 1);
    xTaskCreate(ota_task, "ota_task", 12288, job, 10, NULL);
}

void start_ota_update(const char *url) {
    if (!url) return;
    ota_spawn(url, "");
}

void ota_handle_system_control_record(cJSON *record) {
    if (!record) return;
    cJSON *v_item = cJSON_GetObjectItem(record, "version");
    cJSON *u_item = cJSON_GetObjectItem(record, "bin_url");
    if (!cJSON_IsString(v_item) || !cJSON_IsString(u_item)) return;

    /* Device type targeting: if device_type is specified in the system_control row,
     * it must match our DEVICE_TYPE. If it's missing/different, we ignore the update
     * to prevent cross-flashing different firmwares on a shared database. */
    cJSON *dt_item = cJSON_GetObjectItem(record, "device_type");
    if (cJSON_IsString(dt_item) && dt_item->valuestring[0]) {
        if (strcasecmp(dt_item->valuestring, DEVICE_TYPE) != 0) {
            ESP_LOGI(TAG_OTA, "OTA targeted at device_type %s, not us (%s); skipping",
                     dt_item->valuestring, DEVICE_TYPE);
            return;
        }
    } else {
        ESP_LOGW(TAG_OTA, "OTA system_control record has no device_type; skipping for safety");
        return;
    }

    if (strcmp(v_item->valuestring, FIRMWARE_VERSION) == 0) {
        return; // already on the target version
    }

    /* Canary targeting: if target_device_id is set and isn't us, ignore this
     * trigger so only the canary device updates. NULL/empty = whole fleet. */
    cJSON *t_item = cJSON_GetObjectItem(record, "target_device_id");
    if (cJSON_IsString(t_item) && t_item->valuestring[0]) {
        char my_mac[18];
        get_mac_address(my_mac);
        if (strcasecmp(t_item->valuestring, my_mac) != 0) {
            ESP_LOGI(TAG_OTA, "OTA targeted at %s, not us (%s); skipping",
                     t_item->valuestring, my_mac);
            return;
        }
        ESP_LOGW(TAG_OTA, "Canary OTA targeted at this device (%s)", my_mac);
    }

    // Trim trailing whitespace from URL
    char *clean_url = strdup(u_item->valuestring);
    if (!clean_url) return;
    char *end = clean_url + strlen(clean_url) - 1;
    while (end > clean_url && (*end == ' ' || *end == '\n' || *end == '\r')) {
        *end = '\0';
        end--;
    }

    ESP_LOGW(TAG_OTA, "OTA TRIGGERED! Target version=%s url=%s", v_item->valuestring, clean_url);
    ota_spawn(clean_url, v_item->valuestring);
    free(clean_url);
}

#else /* ENABLE_OTA == 0 : compile out OTA, provide no-op stubs */

void ota_task(void *pvParameter) {
    if (pvParameter) free(pvParameter);
    vTaskDelete(NULL);
}

void start_ota_update(const char *url) {
    (void)url;
    ESP_LOGW(TAG_OTA, "OTA disabled (ENABLE_OTA=0); ignoring update request");
}

void ota_handle_system_control_record(cJSON *record) {
    (void)record;
}

#endif /* ENABLE_OTA */

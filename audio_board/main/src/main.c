#include <stdio.h>
#include "main.h"
#include "wifi.h"
#include "supabase.h"
#include "espnow.h"
#include "mp3board.h"
#include "ota_selftest.h"
#include "crash_report.h"
#include "nvs.h"
#include <time.h>
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_crt_bundle.h"

extern void esp_brownout_disable(void);

#define TAG "Main"
struct timeval tv;
const char* G_REBOOT_REASON_STR = "Unknown";
/* Persisted count of brownout reboots, surfaced in the Supabase heartbeat so a
 * device with bad shop power is visibly flagged. Updated at boot in app_main. */
uint32_t G_BROWNOUT_COUNT = 0;

/**
 * @brief If this boot was a brownout, bump the persisted brownout counter.
 *        Loads the current value into G_BROWNOUT_COUNT either way.
 */
static void update_brownout_counter(esp_reset_reason_t reason)
{
    nvs_handle_t h;
    if (nvs_open("storage", NVS_READWRITE, &h) != ESP_OK) return;
    uint32_t count = 0;
    nvs_get_u32(h, "brownouts", &count);
    if (reason == ESP_RST_BROWNOUT) {
        count++;
        nvs_set_u32(h, "brownouts", count);
        nvs_commit(h);
        ESP_LOGW(TAG, "Brownout reboot #%lu recorded", (unsigned long)count);
    }
    nvs_close(h);
    G_BROWNOUT_COUNT = count;
}

/**
 * @brief Converts the esp_reset_reason_t enum to a human-readable string.
 */
const char* get_reboot_reason_string(esp_reset_reason_t reason)
{
    switch (reason) {
        case ESP_RST_POWERON:      return "Power On";
        case ESP_RST_SW:           return "Software Reset";
        case ESP_RST_PANIC:        return "Panic/Crash";
        case ESP_RST_INT_WDT:      return "Interrupt Watchdog";
        case ESP_RST_TASK_WDT:     return "Task Watchdog";
        case ESP_RST_DEEPSLEEP:    return "Deep Sleep Wakeup";
        case ESP_RST_BROWNOUT:     return "Brownout (Low Voltage)";
        default:                   return "Unknown";
    }
}

/**
 * @brief Background task that brings up networking without blocking the core
 *        announcement/playback path.
 *
 * Offline-first: app_main starts the MP3 board immediately, and
 * this task handles Wi-Fi provisioning, connection, SNTP, and Supabase. If the
 * network is unavailable or never provisioned, only this task is affected — the
 * device keeps working and announcing offline tokens via ESP-NOW.
 */
static void network_task(void *pvParameters)
{
    // --- Network: Wi-Fi provisioning + connect (starts Wi-Fi interface) ---
    wifi_start();

    // ESP-NOW: bring up receiver immediately now that Wi-Fi is started.
    // This runs unconditionally so ESP-NOW works offline-first!
    espnow_init();
    espnow_repin_channel(); // Force the radio to channel 1 immediately

    // Wait for Wi-Fi connection and IP address (may block here indefinitely if offline)
    wifi_wait_for_ip();
    vTaskDelay(pdMS_TO_TICKS(2000)); // settle after IP acquisition

    // Field reliability: report any crash from the previous boot, and run the
    // OTA rollback self-test now that Wi-Fi is up (no-ops on a normal boot).
    crash_report_check_and_upload();
    ota_selftest_run_network_checks();

    initialize_sntp();
    handle_provisioning_timestamp();

    // --- Supabase: realtime OTA websocket + periodic heartbeat ---
    // Run inline in this task's context; supabase_sync_task loops forever.
    supabase_sync_task(NULL);

    // supabase_sync_task never returns, but guard anyway.
    vTaskDelete(NULL);
}


void app_main(void)
{
    // Explicitly disable the hardware brownout detector before performing high-power operations
    esp_brownout_disable();

    esp_reset_reason_t reboot_reason = esp_reset_reason();
    G_REBOOT_REASON_STR = get_reboot_reason_string(reboot_reason);
    ESP_LOGI(TAG, "Last reboot reason: %s", G_REBOOT_REASON_STR);

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        // If NVS partition was truncated or has a new version, erase it and re-init
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Track brownout reboots (bad shop power) and surface the count in heartbeats.
    update_brownout_counter(reboot_reason);

    // OTA rollback: if this is a freshly-OTA'd image, arm the self-test. The
    // network-dependent tiers run later (network_task); this just detects the
    // pending-verify state. No-op on a normal already-validated boot.
    ota_selftest_init();

    ESP_LOGW("DEBUG", "=============================================");
    ESP_LOGW("DEBUG", "SMART AUDIO BOARD (OTA & Realtime Enabled) (%s)", FIRMWARE_VERSION);
    ESP_LOGW("DEBUG", "=============================================");
    print_system_memory_status();

    /* esp_http_client logs an ERROR ("This authentication method is not
     * supported: Bearer") for our Authorization: Bearer <key> header even
     * though it forwards the header correctly and the request succeeds. It is
     * cosmetic noise from the client's built-in auth handler; raise its log
     * threshold to WARN so the bogus ERROR line stops appearing. */
    esp_log_level_set("HTTP_CLIENT", ESP_LOG_WARN);

    // --- Offline-first: core functionality runs regardless of network state ---
    // Start the MP3 playback board FIRST and unconditionally.
    // Receiving ESP-NOW and playing token announcements must work even if Wi-Fi
    // is unprovisioned or disconnected (as long as they share a Wi-Fi channel).
    esp_err_t err = mp3board_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize MP3 board: %s", esp_err_to_name(err));
    }

    // --- Networking runs in the background; it must never block announcements. ---
    xTaskCreate(&network_task, "network", 16384, NULL, 3, NULL);
}

void print_system_memory_status()
{
    ESP_LOGI(TAG, "========== Chip Information ===========================================");
    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);
    const char *chip_model;
    switch (chip_info.model) {
        case CHIP_ESP32: chip_model = "ESP32"; break;
        case CHIP_ESP32S2: chip_model = "ESP32-S2"; break;
        case CHIP_ESP32S3: chip_model = "ESP32-S3"; break;
        case CHIP_ESP32C3: chip_model = "ESP32-C3"; break;
        case CHIP_ESP32H2: chip_model = "ESP32-H2"; break;
        default: chip_model = "Unknown"; break;
    }
    ESP_LOGI(TAG, "Chip model: %s", chip_model);

    ESP_LOGI(TAG,"CPU cores: %d", chip_info.cores);
    ESP_LOGI(TAG,"Silicon revision: %d", chip_info.revision);

    uint32_t flash_size = 0;
    esp_flash_get_size(NULL, &flash_size);
    ESP_LOGI(TAG,"Flash size: %lu MB", (unsigned long) flash_size / (1024 * 1024));

    const esp_app_desc_t *app_desc = esp_app_get_description();
    gettimeofday(&tv, NULL);
    ESP_LOGI(TAG, "========== Program Version ============================================");
    ESP_LOGI(TAG, "[APP] Name: %s", app_desc->project_name);
    ESP_LOGI(TAG, "[APP] Version: %s", app_desc->version);
    ESP_LOGI(TAG, "[APP] Compile Date: %s", app_desc->date);
    ESP_LOGI(TAG, "[APP] Compile Time: %s", app_desc->time);
    ESP_LOGI(TAG, "========== Heap Information ===========================================");
    ESP_LOGI(TAG,"Total free heap: %lu bytes", (unsigned long) heap_caps_get_free_size(MALLOC_CAP_DEFAULT));
    ESP_LOGI(TAG,"Minimum free heap since boot: %lu bytes", (unsigned long) heap_caps_get_minimum_free_size(MALLOC_CAP_DEFAULT));
    ESP_LOGI(TAG,"Internal RAM free: %lu bytes", (unsigned long) heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

    ESP_LOGI(TAG, "========== Stack Information ==========================================");
    ESP_LOGI(TAG,"Current task stack high water mark: %lu bytes", (unsigned long) uxTaskGetStackHighWaterMark(NULL));

    ESP_LOGI(TAG, "========== Flash Partition Information ================================");
    const esp_partition_t *part = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, NULL);
    if (part != NULL) {
        ESP_LOGI(TAG,"App partition size: %lu bytes", (unsigned long) part->size);
    } else {
        ESP_LOGI(TAG,"App partition not found!");
    }

    ESP_LOGI(TAG, "=======================================================================");
}

void initialize_sntp(void)
{
    ESP_LOGI(TAG, "Initializing SNTP");
    /* Set timezone to India Standard Time (UTC+5:30) so log timestamps read IST.
     * POSIX TZ sign is inverted: UTC+5:30 -> "IST-5:30". */
    setenv("TZ", "IST-5:30", 1);
    tzset();
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_init();

    // Wait for time to be set
    time_t now = 0;
    struct tm timeinfo = { 0 };
    int retry = 0;
    const int retry_count = 15;

    while (sntp_get_sync_status() == SNTP_SYNC_STATUS_RESET && ++retry < retry_count) {
        ESP_LOGI(TAG, "Waiting for system time to be set... (%d/%d)", retry, retry_count);
        vTaskDelay(2000 / portTICK_PERIOD_MS);
    }

    time(&now);
    localtime_r(&now, &timeinfo);

    if (timeinfo.tm_year < (2023 - 1900)) {
        ESP_LOGW(TAG, "Time not yet synchronized (offline?); continuing without RTC time.");
    } else {
        ESP_LOGI(TAG, "System time is set.");
    }
}

// Helper to manage the Provisioned Timestamp in NVS
void handle_provisioning_timestamp() {
    nvs_handle_t handle;
    uint8_t new_prov = 0;
    int64_t prov_time = 0;

    if (nvs_open("storage", NVS_READWRITE, &handle) == ESP_OK) {
        // Check if we just provisioned
        nvs_get_u8(handle, "new_prov", &new_prov);

        if (new_prov == 1) {
            time_t now;
            time(&now); // Get current synced time
            prov_time = (int64_t)now;

            nvs_set_i64(handle, "prov_ts", prov_time); // Save actual timestamp
            nvs_set_u8(handle, "new_prov", 0);         // Clear flag
            nvs_commit(handle);
            ESP_LOGW(TAG, "Recorded new provisioning time: %lld", prov_time);
        }
        nvs_close(handle);
    }
}

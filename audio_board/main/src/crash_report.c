#include "crash_report.h"
#include "supabase.h"
#include "main.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <inttypes.h>

#include "esp_log.h"
#include "esp_system.h"
#include "esp_core_dump.h"
#include "cJSON.h"

#define TAG_CR "CrashReport"

/* Set in main.c at boot. */
extern const char *G_REBOOT_REASON_STR;

void crash_report_check_and_upload(void) {
    /* Is there a valid core dump from a previous crash? */
    if (esp_core_dump_image_check() != ESP_OK) {
        return;  /* no dump (clean boot) — nothing to do */
    }

    char crashed_task[32] = "";
    char pc_str[16] = "";
    char bt_str[256] = "";

    esp_core_dump_summary_t *summary = malloc(sizeof(esp_core_dump_summary_t));
    if (summary && esp_core_dump_get_summary(summary) == ESP_OK) {
        strncpy(crashed_task, summary->exc_task, sizeof(crashed_task) - 1);
        snprintf(pc_str, sizeof(pc_str), "0x%08" PRIx32, summary->exc_pc);

        /* Build a space-separated backtrace of PC addresses. */
        size_t off = 0;
        uint32_t depth = summary->exc_bt_info.depth;
        for (uint32_t i = 0; i < depth && off < sizeof(bt_str) - 12; i++) {
            off += snprintf(bt_str + off, sizeof(bt_str) - off,
                            "0x%08" PRIx32 " ", summary->exc_bt_info.bt[i]);
        }
    } else {
        ESP_LOGW(TAG_CR, "core dump present but summary unavailable");
    }
    free(summary);

    char mac_str[18];
    get_mac_address(mac_str);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "p_device_id", mac_str);
    cJSON_AddStringToObject(root, "p_firmware_version", FIRMWARE_VERSION);
    cJSON_AddStringToObject(root, "p_reboot_reason",
                            G_REBOOT_REASON_STR ? G_REBOOT_REASON_STR : "Unknown");
    if (crashed_task[0]) cJSON_AddStringToObject(root, "p_crashed_task", crashed_task);
    else                 cJSON_AddNullToObject(root, "p_crashed_task");
    if (pc_str[0]) cJSON_AddStringToObject(root, "p_fault_pc", pc_str);
    else           cJSON_AddNullToObject(root, "p_fault_pc");
    if (bt_str[0]) cJSON_AddStringToObject(root, "p_backtrace", bt_str);
    else           cJSON_AddNullToObject(root, "p_backtrace");
    cJSON_AddNullToObject(root, "p_uptime_before_crash");

    char *body = cJSON_PrintUnformatted(root);

    ESP_LOGW(TAG_CR, "reporting crash: task=%s pc=%s reason=%s",
             crashed_task[0] ? crashed_task : "?",
             pc_str[0] ? pc_str : "?",
             G_REBOOT_REASON_STR ? G_REBOOT_REASON_STR : "Unknown");

    if (supabase_post_rpc("report_crash", body) == ESP_OK) {
        /* Reported successfully — erase so we don't re-report next boot. */
        esp_core_dump_image_erase();
        ESP_LOGI(TAG_CR, "crash reported + dump erased");
    } else {
        /* Offline / failed: leave the dump in place to retry on a later boot. */
        ESP_LOGW(TAG_CR, "crash upload failed; keeping dump for next boot");
    }

    cJSON_Delete(root);
    free(body);
}

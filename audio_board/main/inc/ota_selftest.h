#ifndef __OTA_SELFTEST_H__
#define __OTA_SELFTEST_H__

#include <stdbool.h>

/**
 * @brief OTA rollback self-test.
 *
 * With CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE, a freshly OTA'd image boots in
 * ESP_OTA_IMG_PENDING_VERIFY and MUST be marked valid or the bootloader reverts
 * to the previous image on the next reset. This module runs a tiered self-test
 * and decides validate vs. roll back:
 *
 *   Tier 1 (HARD): core peripherals (P10 + USB host) initialized.
 *   Tier 2 (HARD): Wi-Fi obtained an IP within a timeout.
 *     -> If either hard tier fails: mark INVALID + reboot (rollback).
 *     -> If both pass: mark VALID (the image is not crash-looping / broken).
 *   Tier 3 (SOFT): Supabase reachable within a grace window.
 *     -> Reported as ota_status 'success' (reachable) or 'degraded' (not),
 *        but NEVER triggers rollback (offline-first: no internet != bad build).
 *
 * If the running image is already valid (normal boot), all calls are no-ops.
 */

/**
 * @brief Note whether peripheral init (P10 + USB host) succeeded.
 *        Called from token_display_start(). Tier-1 input.
 */
void ota_selftest_set_peripherals_ok(bool ok);

/**
 * @brief Start the self-test if this boot is a pending-verify OTA image.
 *        Call early in app_main (after reading reset reason).
 */
void ota_selftest_init(void);

/**
 * @brief Run the network-dependent tiers and finalize validate/rollback.
 *        Call from the network task AFTER Wi-Fi is up. No-op if not a
 *        pending-verify boot.
 */
void ota_selftest_run_network_checks(void);

#endif /* __OTA_SELFTEST_H__ */

#ifndef __CRASH_REPORT_H__
#define __CRASH_REPORT_H__

/**
 * @brief Crash reporting module.
 *
 * On the boot AFTER a crash, reads the ELF core dump saved to the 'coredump'
 * flash partition (CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH), extracts a summary
 * (crashed task, fault PC, backtrace), uploads it to Supabase via the
 * report_crash RPC, then erases the dump so it is reported only once.
 *
 * Call once after Wi-Fi is up (network task). Safe to call when there is no
 * dump (no-op) and when offline (skips upload, leaves the dump for next boot).
 */
void crash_report_check_and_upload(void);

#endif /* __CRASH_REPORT_H__ */

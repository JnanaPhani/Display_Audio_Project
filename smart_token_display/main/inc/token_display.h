#ifndef __TOKEN_DISPLAY_H__
#define __TOKEN_DISPLAY_H__

#include <stdbool.h>
#include <stdint.h>

/**
 * @brief Size of the rolling "last N active tokens" window cycled on the P10
 *        display. The active set is capped at this depth: a 1st-scan token is
 *        appended and, when the window is full, the oldest is evicted (FIFO).
 */
#define SCAN_FIFO_SIZE 5

/**
 * @brief Interval (ms) between cycling to the next value on the display. Also
 *        the solo-hold duration for a freshly scanned number before it rejoins
 *        the loop.
 */
#define DISPLAY_CYCLE_MS 5000

/**
 * @brief Install the USB Host library and start the daemon task.
 *
 * Must be called before barcode_scanner_init(). The application owns the USB
 * Host library lifecycle; this sets up usb_host_install() and the background
 * task that pumps usb_host_lib_handle_events().
 *
 * @return ESP_OK on success, or an esp_err_t from usb_host_install().
 */
int token_display_usb_host_setup(void);

/**
 * @brief Initialize the P10 display, USB-HID barcode scanner, and start the
 *        scan-handler and display tasks.
 *
 * Assumes Wi-Fi/Supabase are (being) brought up separately. Offline tokens run
 * a ready_to_collect -> no_show lifecycle: a 1st scan adds the number to a
 * rolling window of the last SCAN_FIFO_SIZE active tokens (oldest evicted when
 * full) and the display cycles that window; a 2nd scan removes it. Online tokens
 * are logged (packing_complete) but never displayed. Each scan is also posted to
 * Supabase and mirrored to the gateway over ESP-NOW.
 */
void token_display_start(void);

/**
 * @brief Set display brightness level (1..100) and save to NVS.
 */
void token_display_set_brightness(uint8_t percent);

#endif /* __TOKEN_DISPLAY_H__ */

/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * USB HID barcode scanner module for ESP-IDF.
 *
 * Decodes USB HID keyboard-wedge barcode scanners into complete barcode
 * strings. Most USB barcode scanners enumerate as HID keyboards and emit a
 * terminator (ENTER / carriage return) after each scan; this module buffers
 * the keystrokes and emits the assembled string on the terminator.
 *
 * Portability:
 *   - Works on any ESP target with USB-OTG host support (e.g. ESP32-S2,
 *     ESP32-S3, ESP32-P4). The classic ESP32 has no native USB host, so a
 *     build assertion guards unsupported targets.
 *   - The application owns the USB Host library lifecycle (usb_host_install /
 *     usb_host_lib_handle_events). This module only installs and drives the
 *     HID class driver, so it can coexist with other USB host clients.
 *
 * Delivery of a completed barcode (any combination may be used):
 *   - Optional callback invoked from the module task.
 *   - Optional FreeRTOS queue receiving a barcode_scanner_event_t.
 *   - Always: the last value is stored internally and readable via
 *     barcode_scanner_get_last().
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Maximum number of characters in a single barcode (excluding NUL). */
#ifndef BARCODE_SCANNER_MAX_LEN
#define BARCODE_SCANNER_MAX_LEN 128
#endif

/**
 * @brief A completed barcode scan event.
 */
typedef struct {
    char data[BARCODE_SCANNER_MAX_LEN + 1]; /*!< NUL-terminated barcode string. */
    size_t length;                          /*!< Number of characters in data. */
} barcode_scanner_event_t;

/**
 * @brief Callback invoked when a complete barcode has been scanned.
 *
 * Invoked from the HID host background task context. Keep it short; offload
 * heavy work to another task. The pointer is valid only for the duration of
 * the call.
 *
 * @param[in] event Completed barcode scan.
 * @param[in] user_ctx User context supplied in barcode_scanner_config_t.
 */
typedef void (*barcode_scanner_cb_t)(const barcode_scanner_event_t *event,
                                     void *user_ctx);

/**
 * @brief Module configuration.
 *
 * Use BARCODE_SCANNER_DEFAULT_CONFIG() to obtain sane defaults, then override
 * the fields you care about.
 */
typedef struct {
    barcode_scanner_cb_t callback;  /*!< Optional. Called on each complete barcode. May be NULL. */
    void *user_ctx;                 /*!< Optional user context passed to callback. */
    QueueHandle_t out_queue;        /*!< Optional. Receives barcode_scanner_event_t. May be NULL. */
    char terminator;                /*!< Character that ends a barcode (default '\r'). */
    bool keep_terminator;           /*!< If true, the terminator is kept in the stored string. */
    size_t task_priority;           /*!< HID background task priority. */
    size_t task_stack_size;         /*!< HID background task stack size in bytes. */
    BaseType_t task_core_id;        /*!< HID background task core affinity (or tskNO_AFFINITY). */
} barcode_scanner_config_t;

/**
 * @brief Default configuration initializer.
 */
#define BARCODE_SCANNER_DEFAULT_CONFIG()      \
    {                                         \
        .callback = NULL,                     \
        .user_ctx = NULL,                     \
        .out_queue = NULL,                    \
        .terminator = '\r',                   \
        .keep_terminator = false,             \
        .task_priority = 5,                   \
        .task_stack_size = 4096,              \
        .task_core_id = 0,                    \
    }

/**
 * @brief Initialize the barcode scanner module and install the HID driver.
 *
 * The application must have already installed the USB Host library
 * (usb_host_install) before calling this function.
 *
 * @param[in] config Module configuration. Must not be NULL.
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_ARG if config is NULL or invalid
 *      - ESP_ERR_INVALID_STATE if already initialized
 *      - ESP_ERR_NO_MEM on allocation failure
 *      - Other error codes from the HID host driver
 */
esp_err_t barcode_scanner_init(const barcode_scanner_config_t *config);

/**
 * @brief Uninstall the HID driver and release module resources.
 *
 * Does not uninstall the USB Host library; the application owns that.
 *
 * @return
 *      - ESP_OK on success, or if not initialized
 *      - Other error codes from the HID host driver
 */
esp_err_t barcode_scanner_deinit(void);

/**
 * @brief Copy the most recently completed barcode into the caller buffer.
 *
 * Thread-safe. If no barcode has been scanned yet, out_buf is set to an empty
 * string and out_len (if provided) to 0.
 *
 * @param[out] out_buf Destination buffer. Must not be NULL.
 * @param[in] out_buf_size Size of out_buf in bytes (must be > 0).
 * @param[out] out_len Optional. Receives the string length (excluding NUL).
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_ARG if out_buf is NULL or out_buf_size is 0
 *      - ESP_ERR_INVALID_STATE if the module is not initialized
 */
esp_err_t barcode_scanner_get_last(char *out_buf, size_t out_buf_size, size_t *out_len);

#ifdef __cplusplus
}
#endif

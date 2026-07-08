/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * USB HID barcode scanner module implementation.
 */

#include "barcode_scanner.h"

#include <string.h>

#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "usb/hid_host.h"
#include "usb/hid_usage_keyboard.h"

/* This module requires native USB-OTG host support. */
#if !defined(SOC_USB_OTG_SUPPORTED) && !defined(CONFIG_IDF_TARGET_ESP32S2) && \
    !defined(CONFIG_IDF_TARGET_ESP32S3) && !defined(CONFIG_IDF_TARGET_ESP32P4)
#warning "barcode_scanner: target may lack USB-OTG host support; module will not enumerate devices."
#endif

static const char *TAG = "barcode_scanner";

/* ----- Modifier shift mask (left or right shift). ----- */
#define BC_SHIFT_MASK (HID_LEFT_SHIFT | HID_RIGHT_SHIFT)

/**
 * @brief Scancode-to-ASCII translation table.
 *
 * Indexed by HID keyboard usage ID. Column 0 = unshifted, column 1 = shifted.
 * Covers HID_KEY_A (0x04) .. HID_KEY_SLASH (0x38). A value of 0 means the key
 * does not map to a printable character used in barcodes.
 */
static const char s_keycode2ascii[57][2] = {
    {0, 0},   /* 0x00 HID_KEY_NO_PRESS        */
    {0, 0},   /* 0x01 HID_KEY_ROLLOVER        */
    {0, 0},   /* 0x02 HID_KEY_POST_FAIL       */
    {0, 0},   /* 0x03 HID_KEY_ERROR_UNDEFINED */
    {'a', 'A'}, {'b', 'B'}, {'c', 'C'}, {'d', 'D'}, {'e', 'E'},
    {'f', 'F'}, {'g', 'G'}, {'h', 'H'}, {'i', 'I'}, {'j', 'J'},
    {'k', 'K'}, {'l', 'L'}, {'m', 'M'}, {'n', 'N'}, {'o', 'O'},
    {'p', 'P'}, {'q', 'Q'}, {'r', 'R'}, {'s', 'S'}, {'t', 'T'},
    {'u', 'U'}, {'v', 'V'}, {'w', 'W'}, {'x', 'X'}, {'y', 'Y'},
    {'z', 'Z'},
    {'1', '!'}, {'2', '@'}, {'3', '#'}, {'4', '$'}, {'5', '%'},
    {'6', '^'}, {'7', '&'}, {'8', '*'}, {'9', '('}, {'0', ')'},
    {'\r', '\r'}, /* 0x28 HID_KEY_ENTER         */
    {0, 0},       /* 0x29 HID_KEY_ESC           */
    {'\b', '\b'}, /* 0x2A HID_KEY_DEL           */
    {'\t', '\t'}, /* 0x2B HID_KEY_TAB           */
    {' ', ' '},   /* 0x2C HID_KEY_SPACE         */
    {'-', '_'}, {'=', '+'}, {'[', '{'}, {']', '}'}, {'\\', '|'},
    {'\\', '|'},  /* 0x32 HID_KEY_SHARP (NonUS) */
    {';', ':'}, {'\'', '"'}, {'`', '~'}, {',', '<'}, {'.', '>'},
    {'/', '?'},   /* 0x38 HID_KEY_SLASH         */
};

/**
 * @brief Module runtime state.
 */
typedef struct {
    bool initialized;
    barcode_scanner_config_t cfg;

    SemaphoreHandle_t lock;                 /* Protects last_value / last_len. */
    char last_value[BARCODE_SCANNER_MAX_LEN + 1];
    size_t last_len;

    /* In-progress assembly buffer (touched only by the HID task). */
    char assembly[BARCODE_SCANNER_MAX_LEN + 1];
    size_t assembly_len;

    /* Previous keyboard report keys for edge detection. */
    uint8_t prev_keys[HID_KEYBOARD_KEY_MAX];
} barcode_scanner_ctx_t;

static barcode_scanner_ctx_t s_ctx;

/* ------------------------------------------------------------------ */
/* Decoding helpers                                                    */
/* ------------------------------------------------------------------ */

static inline bool bc_is_shift(uint8_t modifier)
{
    return (modifier & BC_SHIFT_MASK) != 0;
}

static inline bool bc_keycode_to_char(uint8_t modifier, uint8_t key_code, char *out_char)
{
    if (key_code < HID_KEY_A || key_code > HID_KEY_SLASH) {
        return false;
    }
    const char c = s_keycode2ascii[key_code][bc_is_shift(modifier) ? 1 : 0];
    if (c == 0) {
        return false;
    }
    *out_char = c;
    return true;
}

static bool bc_key_in_report(const uint8_t *keys, uint8_t key, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        if (keys[i] == key) {
            return true;
        }
    }
    return false;
}

/**
 * @brief Publish a completed barcode through all configured outputs.
 */
static void bc_emit_barcode(void)
{
    barcode_scanner_event_t event;
    event.length = s_ctx.assembly_len;
    memcpy(event.data, s_ctx.assembly, s_ctx.assembly_len);
    event.data[s_ctx.assembly_len] = '\0';

    /* Store as last value (thread-safe). */
    if (xSemaphoreTake(s_ctx.lock, portMAX_DELAY) == pdTRUE) {
        memcpy(s_ctx.last_value, event.data, event.length + 1);
        s_ctx.last_len = event.length;
        xSemaphoreGive(s_ctx.lock);
    }

    ESP_LOGI(TAG, "Scanned (%u): %s", (unsigned)event.length, event.data);

    if (s_ctx.cfg.callback) {
        s_ctx.cfg.callback(&event, s_ctx.cfg.user_ctx);
    }
    if (s_ctx.cfg.out_queue) {
        /* Non-blocking: drop if the consumer is not keeping up. */
        if (xQueueSend(s_ctx.cfg.out_queue, &event, 0) != pdTRUE) {
            ESP_LOGW(TAG, "Output queue full; barcode dropped");
        }
    }

    /* Reset assembly buffer for the next scan. */
    s_ctx.assembly_len = 0;
}

/**
 * @brief Append one decoded character to the in-progress barcode.
 */
static void bc_append_char(char c)
{
    if (c == s_ctx.cfg.terminator) {
        if (s_ctx.cfg.keep_terminator && s_ctx.assembly_len < BARCODE_SCANNER_MAX_LEN) {
            s_ctx.assembly[s_ctx.assembly_len++] = c;
        }
        if (s_ctx.assembly_len > 0) {
            bc_emit_barcode();
        }
        return;
    }

    if (c == '\b') {
        if (s_ctx.assembly_len > 0) {
            s_ctx.assembly_len--;
        }
        return;
    }

    if (s_ctx.assembly_len >= BARCODE_SCANNER_MAX_LEN) {
        ESP_LOGW(TAG, "Barcode exceeds %d chars; flushing", BARCODE_SCANNER_MAX_LEN);
        bc_emit_barcode();
    }
    s_ctx.assembly[s_ctx.assembly_len++] = c;
}

/* ------------------------------------------------------------------ */
/* HID report handling                                                 */
/* ------------------------------------------------------------------ */

/**
 * @brief Decode a boot-protocol keyboard report and feed pressed keys.
 */
static void bc_process_keyboard_report(const uint8_t *data, size_t length)
{
    if (length < sizeof(hid_keyboard_input_report_boot_t)) {
        return;
    }

    const hid_keyboard_input_report_boot_t *report =
        (const hid_keyboard_input_report_boot_t *)data;

    for (int i = 0; i < HID_KEYBOARD_KEY_MAX; i++) {
        const uint8_t key = report->key[i];

        /* Only act on newly pressed keys (key-down edge). */
        if (key > HID_KEY_ERROR_UNDEFINED &&
                !bc_key_in_report(s_ctx.prev_keys, key, HID_KEYBOARD_KEY_MAX)) {
            char c;
            if (bc_keycode_to_char(report->modifier.val, key, &c)) {
                bc_append_char(c);
            }
        }
    }

    memcpy(s_ctx.prev_keys, report->key, HID_KEYBOARD_KEY_MAX);
}

/**
 * @brief HID interface event callback (input reports, disconnect, errors).
 */
static void bc_interface_callback(hid_host_device_handle_t hid_device_handle,
                                  const hid_host_interface_event_t event,
                                  void *arg)
{
    hid_host_dev_params_t dev_params;
    if (hid_host_device_get_params(hid_device_handle, &dev_params) != ESP_OK) {
        return;
    }

    switch (event) {
    case HID_HOST_INTERFACE_EVENT_INPUT_REPORT: {
        uint8_t data[64] = {0};
        size_t data_length = 0;
        if (hid_host_device_get_raw_input_report_data(hid_device_handle, data,
                                                      sizeof(data), &data_length) != ESP_OK) {
            break;
        }
        if (dev_params.sub_class == HID_SUBCLASS_BOOT_INTERFACE &&
                dev_params.proto == HID_PROTOCOL_KEYBOARD) {
            bc_process_keyboard_report(data, data_length);
        }
        /* Non-keyboard interfaces (mouse / generic) are ignored by the
         * barcode module but kept connected so multi-interface scanners
         * still work. */
        break;
    }
    case HID_HOST_INTERFACE_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "HID device disconnected");
        hid_host_device_close(hid_device_handle);
        /* Reset assembly state so a partial scan does not leak across
         * reconnects. */
        s_ctx.assembly_len = 0;
        memset(s_ctx.prev_keys, 0, sizeof(s_ctx.prev_keys));
        break;
    case HID_HOST_INTERFACE_EVENT_TRANSFER_ERROR:
        ESP_LOGW(TAG, "HID transfer error");
        break;
    default:
        ESP_LOGW(TAG, "Unhandled HID interface event %d", (int)event);
        break;
    }
}

/**
 * @brief HID driver event callback (device connected).
 *
 * Runs in the HID host background task. Opens and starts every connected HID
 * interface; configures keyboard interfaces for boot protocol.
 */
static void bc_driver_callback(hid_host_device_handle_t hid_device_handle,
                               const hid_host_driver_event_t event,
                               void *arg)
{
    hid_host_dev_params_t dev_params;
    if (hid_host_device_get_params(hid_device_handle, &dev_params) != ESP_OK) {
        return;
    }

    if (event != HID_HOST_DRIVER_EVENT_CONNECTED) {
        return;
    }

    ESP_LOGI(TAG, "HID device connected (proto=%u)", dev_params.proto);

    const hid_host_device_config_t dev_config = {
        .callback = bc_interface_callback,
        .callback_arg = NULL,
    };

    esp_err_t err = hid_host_device_open(hid_device_handle, &dev_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "hid_host_device_open failed: %s", esp_err_to_name(err));
        return;
    }

    if (dev_params.sub_class == HID_SUBCLASS_BOOT_INTERFACE) {
        hid_class_request_set_protocol(hid_device_handle, HID_REPORT_PROTOCOL_BOOT);
        if (dev_params.proto == HID_PROTOCOL_KEYBOARD) {
            hid_class_request_set_idle(hid_device_handle, 0, 0);
        }
    }

    err = hid_host_device_start(hid_device_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "hid_host_device_start failed: %s", esp_err_to_name(err));
        hid_host_device_close(hid_device_handle);
    }
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

esp_err_t barcode_scanner_init(const barcode_scanner_config_t *config)
{
    ESP_RETURN_ON_FALSE(config != NULL, ESP_ERR_INVALID_ARG, TAG, "config is NULL");
    ESP_RETURN_ON_FALSE(!s_ctx.initialized, ESP_ERR_INVALID_STATE, TAG, "already initialized");

    memset(&s_ctx, 0, sizeof(s_ctx));
    s_ctx.cfg = *config;
    if (s_ctx.cfg.terminator == 0) {
        s_ctx.cfg.terminator = '\r';
    }
    if (s_ctx.cfg.task_stack_size == 0) {
        s_ctx.cfg.task_stack_size = 4096;
    }

    s_ctx.lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_ctx.lock != NULL, ESP_ERR_NO_MEM, TAG, "mutex alloc failed");

    const hid_host_driver_config_t hid_cfg = {
        .create_background_task = true,
        .task_priority = s_ctx.cfg.task_priority,
        .stack_size = s_ctx.cfg.task_stack_size,
        .core_id = s_ctx.cfg.task_core_id,
        .callback = bc_driver_callback,
        .callback_arg = NULL,
    };

    esp_err_t err = hid_host_install(&hid_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "hid_host_install failed: %s", esp_err_to_name(err));
        vSemaphoreDelete(s_ctx.lock);
        s_ctx.lock = NULL;
        return err;
    }

    s_ctx.initialized = true;
    ESP_LOGI(TAG, "Initialized (terminator=0x%02X, max_len=%d)",
             (unsigned)s_ctx.cfg.terminator, BARCODE_SCANNER_MAX_LEN);
    return ESP_OK;
}

esp_err_t barcode_scanner_deinit(void)
{
    if (!s_ctx.initialized) {
        return ESP_OK;
    }

    esp_err_t err = hid_host_uninstall();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "hid_host_uninstall failed: %s", esp_err_to_name(err));
        return err;
    }

    if (s_ctx.lock) {
        vSemaphoreDelete(s_ctx.lock);
    }
    memset(&s_ctx, 0, sizeof(s_ctx));
    ESP_LOGI(TAG, "Deinitialized");
    return ESP_OK;
}

esp_err_t barcode_scanner_get_last(char *out_buf, size_t out_buf_size, size_t *out_len)
{
    ESP_RETURN_ON_FALSE(out_buf != NULL && out_buf_size > 0, ESP_ERR_INVALID_ARG,
                        TAG, "invalid output buffer");
    ESP_RETURN_ON_FALSE(s_ctx.initialized, ESP_ERR_INVALID_STATE, TAG, "not initialized");

    xSemaphoreTake(s_ctx.lock, portMAX_DELAY);
    size_t copy_len = s_ctx.last_len;
    if (copy_len > out_buf_size - 1) {
        copy_len = out_buf_size - 1;
    }
    memcpy(out_buf, s_ctx.last_value, copy_len);
    out_buf[copy_len] = '\0';
    if (out_len) {
        *out_len = copy_len;
    }
    xSemaphoreGive(s_ctx.lock);
    return ESP_OK;
}

/**
 * @file dfplayer.h
 * @brief Portable, instance-based driver for the DFPlayer Mini MP3 module.
 *
 * Transport: half-duplex 3.3V UART, 9600 8N1, 10-byte command/feedback frames.
 *
 * Design goals:
 *  - Portable across every ESP32 variant (WROOM, S2, S3, C3, ...). All
 *    board-specific choices (UART port, TX/RX GPIO, baud) are runtime
 *    configuration, not compile-time macros, so the same firmware image can
 *    drive different wiring and multiple modules can coexist.
 *  - Drop-in: ships as a self-contained ESP-IDF component. Add it to your
 *    project's components/ directory (or via the component manager) and
 *    `REQUIRES dfplayer` in your CMakeLists.
 *  - Reliable: a dedicated RX task owns the UART receive path and decodes
 *    frames with a resynchronising byte state machine. Commands wait for the
 *    module ACK (0x41) and retry; queries return the data reply value;
 *    playback commands surface "cannot play file" errors instead of stopping
 *    silently. Asynchronous notifications (track finished, media inserted,
 *    init) are delivered through an event queue.
 *
 * Threading: all public functions are safe to call from a single owner task.
 * They are NOT re-entrant per handle; serialize calls to one handle yourself
 * if multiple tasks share it. Different handles are fully independent.
 *
 * Example:
 * @code
 *   dfplayer_config_t cfg = DFPLAYER_CONFIG_DEFAULT();
 *   cfg.tx_gpio = 25;
 *   cfg.rx_gpio = 26;
 *   dfplayer_handle_t df = NULL;
 *   ESP_ERROR_CHECK(dfplayer_create(&cfg, &df));
 *   dfplayer_set_volume(df, 20);
 *   dfplayer_play_track(df, 1);
 *   // ...
 *   dfplayer_destroy(df);
 * @endcode
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "driver/uart.h"

#ifdef __cplusplus
extern "C" {
#endif

/** DFPlayer Mini link is always 9600 8N1. */
#define DFPLAYER_DEFAULT_BAUD_RATE 9600

/**
 * @brief Opaque driver instance handle. Create with dfplayer_create().
 */
typedef struct dfplayer_ctx *dfplayer_handle_t;

/** Equalizer presets (command 0x07). */
typedef enum {
    DFPLAYER_EQ_NORMAL  = 0,
    DFPLAYER_EQ_POP     = 1,
    DFPLAYER_EQ_ROCK    = 2,
    DFPLAYER_EQ_JAZZ    = 3,
    DFPLAYER_EQ_CLASSIC = 4,
    DFPLAYER_EQ_BASS    = 5,
} dfplayer_eq_t;

/** Playback source / device (command 0x09). */
typedef enum {
    DFPLAYER_SRC_USB   = 1,
    DFPLAYER_SRC_TF    = 2,   /* microSD card (the usual choice) */
    DFPLAYER_SRC_AUX   = 3,
    DFPLAYER_SRC_SLEEP = 4,
    DFPLAYER_SRC_FLASH = 5,
} dfplayer_source_t;

/** Feedback / event command bytes reported by the module. */
typedef enum {
    DFPLAYER_EVT_MEDIA_INSERTED = 0x3A,
    DFPLAYER_EVT_MEDIA_EJECTED  = 0x3B,
    DFPLAYER_EVT_FINISHED_USB   = 0x3C,
    DFPLAYER_EVT_FINISHED_TF    = 0x3D,
    DFPLAYER_EVT_FINISHED_FLASH = 0x3E,
    DFPLAYER_EVT_INIT_COMPLETE  = 0x3F,
    DFPLAYER_EVT_ERROR          = 0x40,
} dfplayer_event_code_t;

/** Module error codes carried in a 0x40 frame's parameter (low byte). */
typedef enum {
    DFPLAYER_ERR_BUSY            = 0x01, /* module busy / card not found  */
    DFPLAYER_ERR_SLEEPING       = 0x02,
    DFPLAYER_ERR_WRONG_STACK    = 0x03,
    DFPLAYER_ERR_CHECKSUM       = 0x04,
    DFPLAYER_ERR_INDEX_OUT      = 0x05, /* file index out of bound       */
    DFPLAYER_ERR_FILE_MISMATCH  = 0x06, /* cannot find file              */
    DFPLAYER_ERR_IN_ADVERTISE   = 0x07,
} dfplayer_error_code_t;

/**
 * @brief Runtime configuration passed to dfplayer_create().
 *
 * Initialize with DFPLAYER_CONFIG_DEFAULT() then override the fields you need.
 */
typedef struct {
    uart_port_t uart_port;   /**< UART peripheral, e.g. UART_NUM_1 / UART_NUM_2. */
    int         tx_gpio;     /**< ESP TX GPIO -> module RX. Use a free GPIO.     */
    int         rx_gpio;     /**< ESP RX GPIO <- module TX.                      */
    int         baud_rate;   /**< Link baud; keep DFPLAYER_DEFAULT_BAUD_RATE.    */

    uint8_t     ack_retries;     /**< Re-sends if no ACK (default 3).            */
    uint16_t    ack_timeout_ms;  /**< ACK wait per attempt (default 200).        */
    uint16_t    play_error_ms;   /**< Window to catch a play error (default 250).*/
    uint16_t    init_timeout_ms; /**< Wait for 0x3F init at create (default 5000).*/

    bool        reset_on_init;   /**< Send reset (0x0C) during create (default true). */

    int         rx_task_priority;   /**< RX task priority (default 1).           */
    uint32_t    rx_task_stack;      /**< RX task stack bytes (default 3072).     */
    int         rx_task_core;       /**< Pin RX task to core, or -1 = no affinity. */
} dfplayer_config_t;

/**
 * @brief Default configuration.
 *
 * Defaults to UART2 on GPIO25/26 (conflict-free on most ESP32 dev boards;
 * GPIO16/17 are avoided because they are flash/PSRAM-reserved on many
 * WROOM/WROVER modules). Override tx_gpio/rx_gpio/uart_port for your board.
 */
#define DFPLAYER_CONFIG_DEFAULT()                       \
    (dfplayer_config_t){                                \
        .uart_port        = UART_NUM_2,                 \
        .tx_gpio          = 25,                         \
        .rx_gpio          = 26,                         \
        .baud_rate        = DFPLAYER_DEFAULT_BAUD_RATE, \
        .ack_retries      = 3,                          \
        .ack_timeout_ms   = 200,                        \
        .play_error_ms    = 250,                        \
        .init_timeout_ms  = 5000,                       \
        .reset_on_init    = true,                       \
        .rx_task_priority = 1,                          \
        .rx_task_stack    = 3072,                       \
        .rx_task_core     = -1,                         \
    }

/** Parsed feedback/notification frame from the module. */
typedef struct {
    uint8_t  cmd;     /**< Command/event byte (see dfplayer_event_code_t).      */
    uint16_t param;   /**< 16-bit parameter (param_high << 8 | param_low).      */
} dfplayer_event_t;

/* ------------------------------------------------------------------------- */
/* Lifecycle                                                                  */
/* ------------------------------------------------------------------------- */

/**
 * @brief Create and start a DFPlayer driver instance.
 *
 * Installs the UART driver, starts the RX task and (optionally) resets the
 * module, waiting up to cfg->init_timeout_ms for the 0x3F init frame.
 *
 * @param cfg      Configuration (copied internally; may be stack-local).
 * @param out_handle  Receives the new handle on success.
 * @return ESP_OK on success;
 *         ESP_ERR_INVALID_ARG on bad arguments;
 *         ESP_ERR_NO_MEM if allocation fails;
 *         a UART driver error otherwise.
 *         Note: a missing init frame is only a warning, not a failure, so the
 *         handle is still returned (the module may simply be slow / card-less).
 */
esp_err_t dfplayer_create(const dfplayer_config_t *cfg, dfplayer_handle_t *out_handle);

/**
 * @brief Stop the RX task, uninstall the UART driver and free the instance.
 * @param handle Handle from dfplayer_create() (NULL is a no-op).
 */
void dfplayer_destroy(dfplayer_handle_t handle);

/* ------------------------------------------------------------------------- */
/* Playback control                                                           */
/* ------------------------------------------------------------------------- */

esp_err_t dfplayer_play(dfplayer_handle_t h);   /**< Resume / play current.   */
esp_err_t dfplayer_pause(dfplayer_handle_t h);
esp_err_t dfplayer_stop(dfplayer_handle_t h);

/**
 * @brief Skip to next / previous track.
 * @return ESP_OK; ESP_ERR_NOT_FOUND if the module reports it cannot play.
 */
esp_err_t dfplayer_next(dfplayer_handle_t h);
esp_err_t dfplayer_prev(dfplayer_handle_t h);

/**
 * @brief Play a track by global index (1..2999) using command 0x03.
 * @note Index is the FAT directory order, not necessarily the filename number.
 *       For deterministic mapping prefer dfplayer_play_folder().
 * @return ESP_OK; ESP_ERR_NOT_FOUND if the file cannot be played.
 */
esp_err_t dfplayer_play_track(dfplayer_handle_t h, uint16_t track);

/**
 * @brief Play /FF/NNN.mp3 by folder + file number (command 0x0F).
 * @param folder 1..99, @param file 1..255. Order-independent, reliable.
 * @return ESP_OK; ESP_ERR_NOT_FOUND if the file cannot be played.
 */
esp_err_t dfplayer_play_folder(dfplayer_handle_t h, uint8_t folder, uint8_t file);

esp_err_t dfplayer_loop_track(dfplayer_handle_t h, uint16_t track); /**< Repeat one track. */
esp_err_t dfplayer_loop_all(dfplayer_handle_t h, bool enable);
esp_err_t dfplayer_random(dfplayer_handle_t h);
esp_err_t dfplayer_reset(dfplayer_handle_t h);

/* ------------------------------------------------------------------------- */
/* Volume / EQ / source                                                       */
/* ------------------------------------------------------------------------- */

esp_err_t dfplayer_set_volume(dfplayer_handle_t h, uint8_t vol); /**< 0..30. */
esp_err_t dfplayer_volume_up(dfplayer_handle_t h);
esp_err_t dfplayer_volume_down(dfplayer_handle_t h);
esp_err_t dfplayer_set_eq(dfplayer_handle_t h, dfplayer_eq_t eq);
esp_err_t dfplayer_set_source(dfplayer_handle_t h, dfplayer_source_t src);

/* ------------------------------------------------------------------------- */
/* Queries (each writes the module reply value to *out_value)                 */
/* ------------------------------------------------------------------------- */

esp_err_t dfplayer_query_status(dfplayer_handle_t h, uint16_t *out_value);
esp_err_t dfplayer_query_volume(dfplayer_handle_t h, uint16_t *out_value);
esp_err_t dfplayer_query_file_count_tf(dfplayer_handle_t h, uint16_t *out_value);
esp_err_t dfplayer_query_current_track_tf(dfplayer_handle_t h, uint16_t *out_value);

/* ------------------------------------------------------------------------- */
/* Asynchronous events                                                        */
/* ------------------------------------------------------------------------- */

/**
 * @brief Wait for and pop one asynchronous notification (track finished,
 *        media inserted/ejected, init complete, unsolicited error).
 * @param evt        Filled on success.
 * @param timeout_ms Max time to wait.
 * @return ESP_OK; ESP_ERR_TIMEOUT if none arrived; ESP_ERR_INVALID_ARG/STATE.
 */
esp_err_t dfplayer_poll_event(dfplayer_handle_t h, dfplayer_event_t *evt, uint32_t timeout_ms);

/* ------------------------------------------------------------------------- */
/* Helpers (handle-independent)                                               */
/* ------------------------------------------------------------------------- */

/** Human-readable name for a feedback/event command byte (for logging). */
const char *dfplayer_event_name(uint8_t cmd);

/** Human-readable name for a 0x40 module error code (see dfplayer_error_code_t). */
const char *dfplayer_error_name(uint8_t code);

#ifdef __cplusplus
}
#endif

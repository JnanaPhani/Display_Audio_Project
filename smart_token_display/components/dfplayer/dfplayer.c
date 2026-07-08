/**
 * @file dfplayer.c
 * @brief Instance-based DFPlayer Mini UART driver implementation.
 *
 * See dfplayer.h for the public API and design notes. All state lives in a
 * heap-allocated dfplayer_ctx so the driver is fully re-entrant across
 * independent handles and free of module-global variables.
 */
#include "dfplayer.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"

static const char *TAG = "dfplayer";

/* ---- Wire protocol ------------------------------------------------------- */
/* 10-byte frame:
 *   [0] 0x7E start      [5] param high
 *   [1] 0xFF version    [6] param low
 *   [2] 0x06 length     [7] checksum high
 *   [3] command         [8] checksum low
 *   [4] feedback flag   [9] 0xEF end
 */
#define DF_START      0x7E
#define DF_VERSION    0xFF
#define DF_LENGTH     0x06
#define DF_END        0xEF
#define DF_FRAME_LEN  10
#define DF_RX_BUF_SIZE 256

/* Command opcodes used below. */
#define DF_CMD_NEXT        0x01
#define DF_CMD_PREV        0x02
#define DF_CMD_PLAY_TRACK  0x03
#define DF_CMD_VOL_UP      0x04
#define DF_CMD_VOL_DOWN    0x05
#define DF_CMD_SET_VOLUME  0x06
#define DF_CMD_SET_EQ      0x07
#define DF_CMD_LOOP_TRACK  0x08
#define DF_CMD_SET_SOURCE  0x09
#define DF_CMD_RESET       0x0C
#define DF_CMD_PLAY        0x0D
#define DF_CMD_PAUSE       0x0E
#define DF_CMD_PLAY_FOLDER 0x0F
#define DF_CMD_LOOP_ALL    0x11
#define DF_CMD_STOP        0x16
#define DF_CMD_RANDOM      0x18
#define DF_CMD_QUERY_STATUS 0x42
#define DF_CMD_QUERY_VOL    0x43
#define DF_CMD_QUERY_FILES  0x48
#define DF_CMD_QUERY_CURR   0x4C

#define DF_REPLY_ACK   0x41

/**
 * @brief Per-instance driver context.
 */
struct dfplayer_ctx {
    dfplayer_config_t cfg;           /* copy of caller config            */
    QueueHandle_t     reply_q;       /* ACKs + query data replies        */
    QueueHandle_t     event_q;       /* async notifications              */
    TaskHandle_t      rx_task;       /* sole owner of the UART RX path    */
    volatile bool     rx_running;    /* RX task run flag (cleared on stop)*/
};

/* ---- Frame helpers ------------------------------------------------------- */

/** Checksum = two's complement of sum(version..param_low). */
static uint16_t df_checksum(const uint8_t *frame)
{
    uint16_t sum = 0;
    for (int i = 1; i <= 6; i++) {
        sum += frame[i];
    }
    return (uint16_t)(0 - sum);
}

/** Build and transmit one raw frame. No ACK handling. */
static esp_err_t df_tx_frame(dfplayer_handle_t h, uint8_t cmd, bool feedback,
                             uint8_t param_h, uint8_t param_l)
{
    uint8_t frame[DF_FRAME_LEN];
    frame[0] = DF_START;
    frame[1] = DF_VERSION;
    frame[2] = DF_LENGTH;
    frame[3] = cmd;
    frame[4] = feedback ? 0x01 : 0x00;
    frame[5] = param_h;
    frame[6] = param_l;

    uint16_t cs = df_checksum(frame);
    frame[7] = (uint8_t)(cs >> 8);
    frame[8] = (uint8_t)(cs & 0xFF);
    frame[9] = DF_END;

    int written = uart_write_bytes(h->cfg.uart_port, (const char *)frame, DF_FRAME_LEN);
    if (written != DF_FRAME_LEN) {
        ESP_LOGE(TAG, "uart_write_bytes returned %d", written);
        return ESP_FAIL;
    }
    ESP_LOGD(TAG, "TX cmd=0x%02X param=0x%02X%02X", cmd, param_h, param_l);
    return ESP_OK;
}

/* ---- Command / query primitives ----------------------------------------- */

/**
 * Send a command with the feedback flag set and wait for the module ACK
 * (0x41), retrying up to cfg.ack_retries. A 0x40 error fails fast (no retry).
 * Before the queues exist this degrades to a fire-and-forget transmit so it is
 * safe during early init.
 */
static esp_err_t df_send(dfplayer_handle_t h, uint8_t cmd, uint8_t param_h, uint8_t param_l)
{
    if (h->reply_q == NULL) {
        return df_tx_frame(h, cmd, false, param_h, param_l);
    }

    for (int attempt = 0; attempt <= h->cfg.ack_retries; attempt++) {
        /* Drain stale replies so we match THIS command's ACK. */
        dfplayer_event_t stale;
        while (xQueueReceive(h->reply_q, &stale, 0) == pdTRUE) { }

        esp_err_t err = df_tx_frame(h, cmd, true, param_h, param_l);
        if (err != ESP_OK) {
            return err;
        }

        dfplayer_event_t reply;
        if (xQueueReceive(h->reply_q, &reply, pdMS_TO_TICKS(h->cfg.ack_timeout_ms)) == pdTRUE) {
            if (reply.cmd == DF_REPLY_ACK) {
                return ESP_OK;
            }
            if (reply.cmd == DFPLAYER_EVT_ERROR) {
                ESP_LOGE(TAG, "cmd 0x%02X rejected: %s (0x%02X)",
                         cmd, dfplayer_error_name(reply.param & 0xFF), reply.param & 0xFF);
                return ESP_FAIL;
            }
        }
        ESP_LOGW(TAG, "cmd 0x%02X no ACK (attempt %d/%d)",
                 cmd, attempt + 1, h->cfg.ack_retries + 1);
    }
    return ESP_ERR_TIMEOUT;
}

/**
 * Send a query and return its DATA reply parameter. The module sends the data
 * frame (cmd == query) and a 0x41 ACK; we wait through both for the data frame.
 */
static esp_err_t df_query(dfplayer_handle_t h, uint8_t cmd, uint16_t *out_value)
{
    if (h->reply_q == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    for (int attempt = 0; attempt <= h->cfg.ack_retries; attempt++) {
        dfplayer_event_t stale;
        while (xQueueReceive(h->reply_q, &stale, 0) == pdTRUE) { }

        esp_err_t err = df_tx_frame(h, cmd, true, 0, 0);
        if (err != ESP_OK) {
            return err;
        }

        TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(h->cfg.ack_timeout_ms);
        dfplayer_event_t reply;
        while (xTaskGetTickCount() < deadline) {
            TickType_t now = xTaskGetTickCount();
            TickType_t wait = (deadline > now) ? (deadline - now) : 0;
            if (xQueueReceive(h->reply_q, &reply, wait) != pdTRUE) {
                break;
            }
            if (reply.cmd == cmd) {
                if (out_value) {
                    *out_value = reply.param;
                }
                return ESP_OK;
            }
            if (reply.cmd == DFPLAYER_EVT_ERROR) {
                ESP_LOGE(TAG, "query 0x%02X error: %s (0x%02X)",
                         cmd, dfplayer_error_name(reply.param & 0xFF), reply.param & 0xFF);
                return ESP_FAIL;
            }
            /* ACK or stale frame: keep waiting for the data reply. */
        }
        ESP_LOGW(TAG, "query 0x%02X no reply (attempt %d/%d)",
                 cmd, attempt + 1, h->cfg.ack_retries + 1);
    }
    return ESP_ERR_TIMEOUT;
}

/**
 * Send a playback command, confirm the ACK, then watch briefly for an async
 * 0x40 error so a "cannot play file" condition is reported rather than silently
 * stopping. Returns ESP_ERR_NOT_FOUND on such an error.
 */
static esp_err_t df_play_checked(dfplayer_handle_t h, uint8_t cmd, uint8_t param_h, uint8_t param_l)
{
    esp_err_t err = df_send(h, cmd, param_h, param_l);
    if (err != ESP_OK) {
        return err;
    }

    dfplayer_event_t evt;
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(h->cfg.play_error_ms);
    while (xTaskGetTickCount() < deadline) {
        TickType_t now = xTaskGetTickCount();
        uint32_t wait_ms = (deadline > now)
            ? (uint32_t)((deadline - now) * portTICK_PERIOD_MS) : 0;
        if (dfplayer_poll_event(h, &evt, wait_ms) != ESP_OK) {
            break;
        }
        if (evt.cmd == DFPLAYER_EVT_ERROR) {
            ESP_LOGW(TAG, "cmd 0x%02X: cannot play file (%s, code %u)",
                     cmd, dfplayer_error_name(evt.param & 0xFF), evt.param & 0xFF);
            return ESP_ERR_NOT_FOUND;
        }
        /* Ignore unrelated events (finished, media) during the window. */
    }
    return ESP_OK;
}

/* ---- RX path ------------------------------------------------------------- */

/**
 * Read bytes until a well-formed frame (start..end, checksum OK) is decoded or
 * the timeout elapses. A byte-by-byte state machine validates each position so
 * a single bad byte costs at most a 1-byte resync. Only the RX task calls this.
 */
static esp_err_t df_read_frame(dfplayer_handle_t h, dfplayer_event_t *evt, uint32_t timeout_ms)
{
    uint8_t frame[DF_FRAME_LEN];
    int idx = 0;
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);

    while (xTaskGetTickCount() < deadline) {
        uint8_t b;
        int n = uart_read_bytes(h->cfg.uart_port, &b, 1, pdMS_TO_TICKS(10));
        if (n <= 0) {
            continue;
        }

        switch (idx) {
            case 0: /* start */
                if (b == DF_START) frame[idx++] = b;
                break;
            case 1: /* version */
                if (b == DF_VERSION) frame[idx++] = b;
                else idx = (b == DF_START) ? 1 : 0;
                break;
            case 2: /* length */
                if (b == DF_LENGTH) frame[idx++] = b;
                else idx = (b == DF_START) ? 1 : 0;
                break;
            default: /* 3..8: cmd, feedback, params, checksum */
                frame[idx++] = b;
                break;
            case 9: /* end byte completes the frame */
                if (b != DF_END) {
                    idx = (b == DF_START) ? 1 : 0;
                    if (idx == 1) frame[0] = DF_START;
                    break;
                }
                frame[9] = b;
                uint16_t cs  = df_checksum(frame);
                uint16_t got = ((uint16_t)frame[7] << 8) | frame[8];
                idx = 0;
                if (cs != got) {
                    ESP_LOGW(TAG, "checksum mismatch calc=0x%04X got=0x%04X", cs, got);
                    break;
                }
                evt->cmd   = frame[3];
                evt->param = ((uint16_t)frame[5] << 8) | frame[6];
                return ESP_OK;
        }
    }
    return ESP_ERR_TIMEOUT;
}

/**
 * RX task: sole owner of the UART receive path. Routes ACK (0x41) and query
 * replies (0x42..0x4F) to reply_q; async notifications to event_q. A 0x40 error
 * goes to BOTH so the command path can fail fast and the app can observe it.
 * Yields on every loop so it can never starve equal-priority tasks (which would
 * trip the startup watchdog while the module auto-streams bytes).
 */
static void df_rx_task(void *arg)
{
    dfplayer_handle_t h = (dfplayer_handle_t)arg;
    dfplayer_event_t evt;

    while (h->rx_running) {
        if (df_read_frame(h, &evt, 1000) != ESP_OK) {
            vTaskDelay(1);
            continue;
        }

        bool is_reply = (evt.cmd == DF_REPLY_ACK) ||
                        (evt.cmd >= 0x42 && evt.cmd <= 0x4F);

        if (evt.cmd == DFPLAYER_EVT_ERROR) {
            xQueueSend(h->reply_q, &evt, 0);
            xQueueSend(h->event_q, &evt, 0);
        } else if (is_reply) {
            xQueueSend(h->reply_q, &evt, 0);
        } else {
            xQueueSend(h->event_q, &evt, 0);
        }
        taskYIELD();
    }

    /* Signal df_destroy that we have exited, then delete self. */
    h->rx_task = NULL;
    vTaskDelete(NULL);
}

/* ---- Lifecycle ----------------------------------------------------------- */

esp_err_t dfplayer_create(const dfplayer_config_t *cfg, dfplayer_handle_t *out_handle)
{
    if (cfg == NULL || out_handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    dfplayer_handle_t h = calloc(1, sizeof(struct dfplayer_ctx));
    if (h == NULL) {
        return ESP_ERR_NO_MEM;
    }
    h->cfg = *cfg;

    const uart_config_t uart_cfg = {
        .baud_rate  = h->cfg.baud_rate,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    esp_err_t err = uart_driver_install(h->cfg.uart_port, DF_RX_BUF_SIZE, 0, 0, NULL, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_driver_install failed: %s", esp_err_to_name(err));
        goto fail_free;
    }
    err = uart_param_config(h->cfg.uart_port, &uart_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_param_config failed: %s", esp_err_to_name(err));
        goto fail_uart;
    }
    err = uart_set_pin(h->cfg.uart_port, h->cfg.tx_gpio, h->cfg.rx_gpio,
                       UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_set_pin failed: %s", esp_err_to_name(err));
        goto fail_uart;
    }

    ESP_LOGI(TAG, "UART%d ready (TX=%d RX=%d @ %d baud)",
             h->cfg.uart_port, h->cfg.tx_gpio, h->cfg.rx_gpio, h->cfg.baud_rate);

    h->reply_q = xQueueCreate(4, sizeof(dfplayer_event_t));
    h->event_q = xQueueCreate(8, sizeof(dfplayer_event_t));
    if (h->reply_q == NULL || h->event_q == NULL) {
        ESP_LOGE(TAG, "queue alloc failed");
        err = ESP_ERR_NO_MEM;
        goto fail_queues;
    }

    h->rx_running = true;
    BaseType_t ok;
    if (h->cfg.rx_task_core >= 0) {
        ok = xTaskCreatePinnedToCore(df_rx_task, "df_rx", h->cfg.rx_task_stack, h,
                                     h->cfg.rx_task_priority, &h->rx_task,
                                     h->cfg.rx_task_core);
    } else {
        ok = xTaskCreate(df_rx_task, "df_rx", h->cfg.rx_task_stack, h,
                         h->cfg.rx_task_priority, &h->rx_task);
    }
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "rx task create failed");
        h->rx_running = false;
        err = ESP_ERR_NO_MEM;
        goto fail_queues;
    }

    /* Discard power-up noise on the module's TX line. */
    uart_flush_input(h->cfg.uart_port);

    /* Optionally reset and wait for the 0x3F init frame. A missing init frame
     * is a warning, not a failure (slow / card-less modules still work). */
    if (h->cfg.reset_on_init) {
        df_tx_frame(h, DF_CMD_RESET, false, 0, 0);

        dfplayer_event_t evt;
        bool got_init = false;
        int slices = (h->cfg.init_timeout_ms + 999) / 1000;
        for (int i = 0; i < slices; i++) {
            if (dfplayer_poll_event(h, &evt, 1000) == ESP_OK &&
                evt.cmd == DFPLAYER_EVT_INIT_COMPLETE) {
                ESP_LOGI(TAG, "init complete: media online mask=0x%02X", evt.param & 0xFF);
                got_init = true;
                break;
            }
        }
        if (!got_init) {
            ESP_LOGW(TAG, "no init (0x3F) frame seen - check wiring/SD card; continuing");
        }
        uart_flush_input(h->cfg.uart_port);
    }

    *out_handle = h;
    return ESP_OK;

fail_queues:
    if (h->reply_q) vQueueDelete(h->reply_q);
    if (h->event_q) vQueueDelete(h->event_q);
fail_uart:
    uart_driver_delete(h->cfg.uart_port);
fail_free:
    free(h);
    return err;
}

void dfplayer_destroy(dfplayer_handle_t h)
{
    if (h == NULL) {
        return;
    }

    /* Ask the RX task to exit and wait for it to self-delete. */
    h->rx_running = false;
    for (int i = 0; i < 200 && h->rx_task != NULL; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    uart_driver_delete(h->cfg.uart_port);
    if (h->reply_q) vQueueDelete(h->reply_q);
    if (h->event_q) vQueueDelete(h->event_q);
    free(h);
}

/* ---- Playback ------------------------------------------------------------ */

esp_err_t dfplayer_play(dfplayer_handle_t h)  { return df_send(h, DF_CMD_PLAY, 0, 0); }
esp_err_t dfplayer_pause(dfplayer_handle_t h) { return df_send(h, DF_CMD_PAUSE, 0, 0); }
esp_err_t dfplayer_stop(dfplayer_handle_t h)  { return df_send(h, DF_CMD_STOP, 0, 0); }
esp_err_t dfplayer_next(dfplayer_handle_t h)  { return df_play_checked(h, DF_CMD_NEXT, 0, 0); }
esp_err_t dfplayer_prev(dfplayer_handle_t h)  { return df_play_checked(h, DF_CMD_PREV, 0, 0); }
esp_err_t dfplayer_random(dfplayer_handle_t h){ return df_send(h, DF_CMD_RANDOM, 0, 0); }
esp_err_t dfplayer_reset(dfplayer_handle_t h) { return df_send(h, DF_CMD_RESET, 0, 0); }

esp_err_t dfplayer_play_track(dfplayer_handle_t h, uint16_t track)
{
    return df_play_checked(h, DF_CMD_PLAY_TRACK, (uint8_t)(track >> 8), (uint8_t)(track & 0xFF));
}

esp_err_t dfplayer_loop_track(dfplayer_handle_t h, uint16_t track)
{
    return df_play_checked(h, DF_CMD_LOOP_TRACK, (uint8_t)(track >> 8), (uint8_t)(track & 0xFF));
}

esp_err_t dfplayer_play_folder(dfplayer_handle_t h, uint8_t folder, uint8_t file)
{
    return df_play_checked(h, DF_CMD_PLAY_FOLDER, folder, file);
}

esp_err_t dfplayer_loop_all(dfplayer_handle_t h, bool enable)
{
    return df_send(h, DF_CMD_LOOP_ALL, 0, enable ? 0x01 : 0x00);
}

/* ---- Volume / EQ / source ------------------------------------------------ */

esp_err_t dfplayer_set_volume(dfplayer_handle_t h, uint8_t vol)
{
    if (vol > 30) vol = 30;
    return df_send(h, DF_CMD_SET_VOLUME, 0, vol);
}

esp_err_t dfplayer_volume_up(dfplayer_handle_t h)   { return df_send(h, DF_CMD_VOL_UP, 0, 0); }
esp_err_t dfplayer_volume_down(dfplayer_handle_t h) { return df_send(h, DF_CMD_VOL_DOWN, 0, 0); }

esp_err_t dfplayer_set_eq(dfplayer_handle_t h, dfplayer_eq_t eq)
{
    return df_send(h, DF_CMD_SET_EQ, 0, (uint8_t)eq);
}

esp_err_t dfplayer_set_source(dfplayer_handle_t h, dfplayer_source_t src)
{
    return df_send(h, DF_CMD_SET_SOURCE, 0, (uint8_t)src);
}

/* ---- Queries ------------------------------------------------------------- */

esp_err_t dfplayer_query_status(dfplayer_handle_t h, uint16_t *out_value)
{
    return df_query(h, DF_CMD_QUERY_STATUS, out_value);
}
esp_err_t dfplayer_query_volume(dfplayer_handle_t h, uint16_t *out_value)
{
    return df_query(h, DF_CMD_QUERY_VOL, out_value);
}
esp_err_t dfplayer_query_file_count_tf(dfplayer_handle_t h, uint16_t *out_value)
{
    return df_query(h, DF_CMD_QUERY_FILES, out_value);
}
esp_err_t dfplayer_query_current_track_tf(dfplayer_handle_t h, uint16_t *out_value)
{
    return df_query(h, DF_CMD_QUERY_CURR, out_value);
}

/* ---- Events -------------------------------------------------------------- */

esp_err_t dfplayer_poll_event(dfplayer_handle_t h, dfplayer_event_t *evt, uint32_t timeout_ms)
{
    if (h == NULL || evt == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (h->event_q == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xQueueReceive(h->event_q, evt, pdMS_TO_TICKS(timeout_ms)) == pdTRUE) {
        return ESP_OK;
    }
    return ESP_ERR_TIMEOUT;
}

/* ---- Name helpers -------------------------------------------------------- */

const char *dfplayer_event_name(uint8_t cmd)
{
    switch (cmd) {
        case DFPLAYER_EVT_MEDIA_INSERTED: return "medium-inserted";
        case DFPLAYER_EVT_MEDIA_EJECTED:  return "medium-ejected";
        case DFPLAYER_EVT_FINISHED_USB:   return "finished-usb";
        case DFPLAYER_EVT_FINISHED_TF:    return "finished-tf";
        case DFPLAYER_EVT_FINISHED_FLASH: return "finished-flash";
        case DFPLAYER_EVT_INIT_COMPLETE:  return "init-complete";
        case DFPLAYER_EVT_ERROR:          return "error";
        case DF_REPLY_ACK:                return "ack";
        case DF_CMD_QUERY_STATUS:         return "status";
        case DF_CMD_QUERY_VOL:            return "volume";
        case DF_CMD_QUERY_FILES:          return "tf-file-count";
        case DF_CMD_QUERY_CURR:           return "tf-current-track";
        default:                          return "unknown";
    }
}

const char *dfplayer_error_name(uint8_t code)
{
    switch (code) {
        case DFPLAYER_ERR_BUSY:           return "module busy / card not found";
        case DFPLAYER_ERR_SLEEPING:       return "sleeping";
        case DFPLAYER_ERR_WRONG_STACK:    return "serial wrong stack";
        case DFPLAYER_ERR_CHECKSUM:       return "checksum mismatch";
        case DFPLAYER_ERR_INDEX_OUT:      return "file index out of bound";
        case DFPLAYER_ERR_FILE_MISMATCH:  return "cannot find file";
        case DFPLAYER_ERR_IN_ADVERTISE:   return "in advertise";
        default:                          return "unknown error";
    }
}

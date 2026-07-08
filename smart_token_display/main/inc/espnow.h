#ifndef __ESPNOW_H__
#define __ESPNOW_H__

#include <stdint.h>
#include "esp_err.h"

/**
 * @brief ESP-NOW scan-broadcast module.
 *
 * Each accepted barcode scan is unicast to a gateway ESP ("announcer") in
 * addition to the existing Supabase upload. The two paths are independent: the
 * ESP-NOW path keeps working even when the shop router is off, as long as every
 * device (the 6 senders + the gateway) shares the SAME Wi-Fi channel.
 *
 * Compiled and active only when ENABLE_ESPNOW (see main.h) is 1. When it is 0,
 * all public functions become no-ops so callers compile unconditionally.
 *
 * ----------------------------------------------------------------------------
 * SINGLE-CHANNEL RULE (critical for deployment):
 *   The ESP32 radio is on one channel at a time. While the STA is connected to
 *   the router, the radio sits on the ROUTER'S channel, and ESP-NOW rides on it
 *   (peer channel 0 = "current channel"). For ESP-NOW to survive a router
 *   outage, the WHOLE fleet must use a FIXED channel:
 *     * Lock the shop router's 2.4 GHz band to ESPNOW_CHANNEL.
 *     * The gateway is permanently pinned to ESPNOW_CHANNEL.
 *     * On STA disconnect, senders re-pin to ESPNOW_CHANNEL via
 *       espnow_repin_channel() so announcements keep flowing with no router.
 *   ESPNOW_CHANNEL and the router's fixed channel MUST be equal.
 * ----------------------------------------------------------------------------
 */

/**
 * @brief Fixed Wi-Fi channel shared by all senders + the gateway.
 *
 * MUST equal the shop router's fixed 2.4 GHz channel (1, 6, or 11 recommended).
 * Default 1 is a placeholder until the router is locked to a chosen channel.
 */
#ifndef ESPNOW_CHANNEL
#define ESPNOW_CHANNEL 1
#endif

/** @brief Wire-format version of espnow_scan_msg_t (bump on layout change). */
#define ESPNOW_PROTO_VERSION 1

/** @brief order_type byte values. */
enum {
    ESPNOW_ORDER_OFFLINE = 0,
    ESPNOW_ORDER_ONLINE  = 1,
};

/** @brief status byte values. */
enum {
    ESPNOW_STATUS_READY_TO_COLLECT = 0,
    ESPNOW_STATUS_NO_SHOW          = 1,
    ESPNOW_STATUS_PACKING_COMPLETE = 2,
};

/**
 * @brief One scan event sent over the air. Packed, fixed-size, ~96 bytes
 *        (well within ESP_NOW_MAX_DATA_LEN = 250).
 */
typedef struct __attribute__((packed)) {
    uint8_t  version;        /* = ESPNOW_PROTO_VERSION                      */
    uint8_t  order_type;     /* ESPNOW_ORDER_*                              */
    uint8_t  status;         /* ESPNOW_STATUS_*                             */
    uint8_t  _pad;           /* alignment / reserved                        */
    int32_t  display_num;    /* numeric token (offline); -1 for online      */
    char     token[24];      /* decoded token ("42" / "ORD42")              */
    char     order_id[40];   /* raw scanned barcode (truncated to fit)      */
    char     event_date[12]; /* "YYYY-MM-DD" or ""                          */
    uint8_t  device_mac[6];  /* sender STA MAC (which device scanned)       */
    uint32_t seq;            /* per-boot increasing counter (dedup/debug)   */
} espnow_scan_msg_t;

/**
 * @brief Initialize ESP-NOW and register the gateway peer.
 *
 * Must be called AFTER Wi-Fi is started (esp_wifi_start done). Idempotent.
 * No-op when ENABLE_ESPNOW == 0.
 *
 * @return ESP_OK on success (or when disabled), else an esp_err_t.
 */
esp_err_t espnow_init(void);

/**
 * @brief Best-effort unicast of a scan event to the gateway.
 *
 * Non-blocking and failure-tolerant: logs SUCCESS/FAIL but never retries or
 * blocks the scan path. Safe to call before espnow_init() (no-ops until ready)
 * and when ENABLE_ESPNOW == 0.
 *
 * The caller fills every field except version/device_mac/seq, which this
 * function stamps. Pass a fully-populated payload otherwise.
 *
 * @param msg Scan message to send (must be non-NULL).
 */
void espnow_send_scan(espnow_scan_msg_t *msg);

/**
 * @brief Re-pin the radio to ESPNOW_CHANNEL.
 *
 * Call on WIFI_EVENT_STA_DISCONNECTED so ESP-NOW keeps working when the router
 * is down (the STA would otherwise wander channels while scanning to reconnect).
 * No-op when ENABLE_ESPNOW == 0.
 */
void espnow_repin_channel(void);

#endif /* __ESPNOW_H__ */

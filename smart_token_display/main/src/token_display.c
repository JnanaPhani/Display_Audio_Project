#include "token_display.h"

#include <ctype.h>
#include <string.h>
#include <time.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "barcode_scanner.h"
#include "espnow.h"
#include "ota_selftest.h"
#include "p10display.h"
#include "supabase.h"
#include "usb/usb_host.h"

#define TAG_TD "token_display"

/* ================================================================== */
/* Domain model                                                        */
/* ------------------------------------------------------------------ */
/* Barcode format:  BY-<YYYYMMDD>-T<token>                             */
/*   * offline/walk-in : token all digits  (e.g. BY-20260615-T42  -> "42")   */
/*   * online order    : token has letters (e.g. BY-20260615-TORD42 -> "ORD42")
 */
/*                                                                     */
/* Lifecycle / DB status:                                              */
/*   * online, every scan       -> "packing_complete"  (never displayed)     */
/*   * offline, 1st scan        -> "ready_to_collect"  (number shown on P10) */
/*   * offline, 2nd scan        -> "no_show"           (number removed; done)*/
/*                                                                     */
/* Local state (NVS-persisted, survives reboot):                       */
/*   * active[] : offline tokens currently shown on the display              */
/*   * done[]   : offline tokens already no-show'd this session (ignored)     */
/* Reset: cleared on successful flush-to-DB, and wiped every 6 hours.        */
/* ================================================================== */

#define TOKEN_MAXLEN 24 /* decoded token string max ("ORD....") */
#define ORDERID_MAXLEN (BARCODE_SCANNER_MAX_LEN + 1)
#define ACTIVE_MAX SCAN_FIFO_SIZE /* rolling window: last N active tokens */
#define DONE_MAX 64               /* max no-show'd tokens remembered       */
#define SESSION_WIPE_US (6ULL * 60 * 60 * 1000000ULL) /* 6 hours */
/* DISPLAY_CYCLE_MS comes from token_display.h */

/* ------------------------------------------------------------------ */
/* Bad-barcode detection toggle                                        */
/* ------------------------------------------------------------------ */
/* When 1 (default): barcodes that don't match the BY-YYYYMMDD-T<token>     */
/*   format are rejected and logged.                                        */
/* When 0: format checking is relaxed. A barcode that fails the BY-... parse */
/*   is treated as an offline/walk-in token using its last-4 digits, so any  */
/*   numeric barcode still drives the display lifecycle. Non-numeric junk is  */
/*   still ignored (there is nothing to show).                               */
#ifndef BARCODE_STRICT_FORMAT
#define BARCODE_STRICT_FORMAT 0
#endif

/* ------------------------------------------------------------------ */
/* Offline-first event queue (NVS-backed ring buffer)                  */
/* ------------------------------------------------------------------ */
#define OFFLINE_NS "scanq"
#define OFFLINE_KEY_HEAD "head"
#define OFFLINE_KEY_TAIL "tail"
#define OFFLINE_QUEUE_MAX 64
#define OFFLINE_FLUSH_MS 5000

/* NVS namespace + keys for the persisted local lifecycle state. */
#define STATE_NS "tstate"
#define STATE_KEY_ACTIVE "active"
#define STATE_KEY_DONE "done"

/* One queued DB event awaiting upload. */
typedef struct {
  char order_id[ORDERID_MAXLEN];
  char token[TOKEN_MAXLEN];
  char order_type[8];  /* "online" | "offline" */
  char status[20];     /* packing_complete | ready_to_collect | no_show */
  char event_date[12]; /* YYYY-MM-DD or "" */
} offline_event_t;

/* One active (waiting) offline token shown on the display. */
typedef struct {
  char order_id[ORDERID_MAXLEN];
  int display_num; /* numeric token shown on P10 */
} active_token_t;

/* A no-show'd offline token (permanently done for this session). */
typedef struct {
  char order_id[ORDERID_MAXLEN];
} done_token_t;

/* ------------------------------------------------------------------ */
/* In-RAM local state (mirror of NVS), guarded by s_state_lock.        */
/* ------------------------------------------------------------------ */
static active_token_t s_active[ACTIVE_MAX];
static int s_active_count;
static done_token_t s_done[DONE_MAX];
static int s_done_count;
static SemaphoreHandle_t s_state_lock;

/* Queue receiving completed barcodes from the scanner component. */
static QueueHandle_t s_scan_queue;

/* Display task handle + "show this number NOW" request. When a fresh offline
 * token is scanned, handle_scan() publishes the number here and notifies the
 * display task, which shows it immediately (no waiting on the cycle delay) and
 * only then resumes the round-robin cycle. */
static TaskHandle_t s_display_task;
static volatile int s_priority_num = -1; /* -1 = no pending priority show */

/* Forward decl: defined with the display task below, used in handle_scan(). */
static void display_show_now(int num);

/* ================================================================== */
/* Barcode parsing                                                     */
/* ================================================================== */
typedef struct {
  bool valid;
  bool is_online;           /* true = online order (token has letters) */
  char token[TOKEN_MAXLEN]; /* decoded token, e.g. "42" or "ORD42" */
  char date_iso[12];        /* "YYYY-MM-DD" */
  int display_num;          /* numeric token for offline; -1 for online */
} parsed_barcode_t;

/**
 * @brief Take up to the last 4 digits of a string, ignoring non-digits.
 *
 * Used by relaxed (non-strict) mode to derive a display number from an
 * arbitrary barcode. Returns 0..9999, or -1 if the string has no digits.
 */
static int parse_last4(const char *s) {
  char digits[16];
  int n = 0;
  for (; *s; s++) {
    if (*s >= '0' && *s <= '9') {
      if (n < (int)sizeof(digits)) {
        digits[n++] = *s;
      } else {
        memmove(digits, digits + 1, sizeof(digits) - 1);
        digits[sizeof(digits) - 1] = *s;
      }
    }
  }
  if (n == 0)
    return -1;
  int start = n > 4 ? n - 4 : 0;
  int v = 0;
  for (int i = start; i < n; i++) {
    v = v * 10 + (digits[i] - '0');
  }
  return v;
}

/**
 * @brief Parse "BY-YYYYMMDD-T<token>" into its fields.
 *
 * Returns valid=false if the prefix, date block, or "-T" marker is malformed.
 * Online vs offline is decided by whether <token> is purely numeric.
 */
static parsed_barcode_t parse_barcode(const char *s) {
  parsed_barcode_t out;
  memset(&out, 0, sizeof(out));
  out.display_num = -1;

  /* Expect prefix "BY-". */
  if (strncmp(s, "BY-", 3) != 0)
    return out;
  const char *p = s + 3;

  /* 8 date digits YYYYMMDD. */
  for (int i = 0; i < 8; i++) {
    if (!isdigit((unsigned char)p[i]))
      return out;
  }
  /* Then "-T". */
  if (p[8] != '-' || p[9] != 'T')
    return out;

  char yyyy[5] = {0}, mm[3] = {0}, dd[3] = {0};
  memcpy(yyyy, p, 4);
  memcpy(mm, p + 4, 2);
  memcpy(dd, p + 6, 2);
  snprintf(out.date_iso, sizeof(out.date_iso), "%s-%s-%s", yyyy, mm, dd);

  /* Token = everything after "-T". Must be non-empty. */
  const char *tok = p + 10;
  if (tok[0] == '\0')
    return out;
  if (strlen(tok) >= TOKEN_MAXLEN)
    return out;
  strncpy(out.token, tok, TOKEN_MAXLEN - 1);

  /* Online if any non-digit char appears in the token. */
  bool all_digits = true;
  for (const char *q = tok; *q; q++) {
    if (!isdigit((unsigned char)*q)) {
      all_digits = false;
      break;
    }
  }
  out.is_online = !all_digits;
  if (!out.is_online) {
    out.display_num = atoi(tok); /* numeric token to show on the panel */
  }
  out.valid = true;
  return out;
}

/** @brief Today's date as "YYYY-MM-DD", or "" if time isn't synced yet. */
static void today_iso(char *buf, size_t n) {
  buf[0] = '\0';
  time_t now = time(NULL);
  struct tm tm_now;
  localtime_r(&now, &tm_now);
  if (tm_now.tm_year >= (2023 - 1900)) { /* time looks valid (synced) */
    strftime(buf, n, "%Y-%m-%d", &tm_now);
  }
}

/* ================================================================== */
/* NVS persistence of the active/done sets                             */
/* ================================================================== */
static void state_save(void) {
  nvs_handle_t h;
  if (nvs_open(STATE_NS, NVS_READWRITE, &h) != ESP_OK)
    return;
  nvs_set_blob(h, STATE_KEY_ACTIVE, s_active,
               sizeof(active_token_t) * s_active_count);
  nvs_set_u32(h, "acount", (uint32_t)s_active_count);
  nvs_set_blob(h, STATE_KEY_DONE, s_done, sizeof(done_token_t) * s_done_count);
  nvs_set_u32(h, "dcount", (uint32_t)s_done_count);
  nvs_commit(h);
  nvs_close(h);
}

static void state_load(void) {
  s_active_count = 0;
  s_done_count = 0;
  nvs_handle_t h;
  if (nvs_open(STATE_NS, NVS_READONLY, &h) != ESP_OK)
    return;

  uint32_t ac = 0, dc = 0;
  nvs_get_u32(h, "acount", &ac);
  nvs_get_u32(h, "dcount", &dc);
  if (ac > ACTIVE_MAX)
    ac = ACTIVE_MAX;
  if (dc > DONE_MAX)
    dc = DONE_MAX;

  size_t len = sizeof(active_token_t) * ac;
  if (ac > 0 && nvs_get_blob(h, STATE_KEY_ACTIVE, s_active, &len) == ESP_OK) {
    s_active_count = (int)ac;
  }
  len = sizeof(done_token_t) * dc;
  if (dc > 0 && nvs_get_blob(h, STATE_KEY_DONE, s_done, &len) == ESP_OK) {
    s_done_count = (int)dc;
  }
  nvs_close(h);
  ESP_LOGI(TAG_TD, "state loaded: %d active, %d done", s_active_count,
           s_done_count);
}

/* Clear all local lifecycle state (RAM + NVS). Caller must hold s_state_lock.
 */
static void state_clear_locked(void) {
  s_active_count = 0;
  s_done_count = 0;
  nvs_handle_t h;
  if (nvs_open(STATE_NS, NVS_READWRITE, &h) == ESP_OK) {
    nvs_erase_all(h);
    nvs_commit(h);
    nvs_close(h);
  }
}

/* ------------------------------------------------------------------ */
/* Active/done set helpers (caller holds s_state_lock)                 */
/* ------------------------------------------------------------------ */
static int active_find(const char *order_id) {
  for (int i = 0; i < s_active_count; i++) {
    if (strcmp(s_active[i].order_id, order_id) == 0)
      return i;
  }
  return -1;
}

static bool done_contains(const char *order_id) {
  for (int i = 0; i < s_done_count; i++) {
    if (strcmp(s_done[i].order_id, order_id) == 0)
      return true;
  }
  return false;
}

static void done_add(const char *order_id); /* fwd decl: used by active_add() */

static bool active_add(const char *order_id, int display_num) {
  /* Rolling last-N window: when full, evict the OLDEST entry (FIFO) to make
   * room for the new one. The evicted token is marked done so a later re-scan
   * is ignored rather than reappearing in the loop. */
  if (s_active_count >= ACTIVE_MAX) {
    done_add(s_active[0].order_id);
    ESP_LOGI(TAG_TD, "active window full; evicting oldest '%s'",
             s_active[0].order_id);
    memmove(&s_active[0], &s_active[1],
            sizeof(active_token_t) * (ACTIVE_MAX - 1));
    s_active_count = ACTIVE_MAX - 1;
  }
  strncpy(s_active[s_active_count].order_id, order_id, ORDERID_MAXLEN - 1);
  s_active[s_active_count].order_id[ORDERID_MAXLEN - 1] = '\0';
  s_active[s_active_count].display_num = display_num;
  s_active_count++;
  return true;
}

static void active_remove_at(int idx) {
  if (idx < 0 || idx >= s_active_count)
    return;
  /* Shift-down compaction: preserve FIFO (oldest->newest) order so the display
   * cycles predictably. */
  memmove(&s_active[idx], &s_active[idx + 1],
          sizeof(active_token_t) * (s_active_count - idx - 1));
  s_active_count--;
}

static void done_add(const char *order_id) {
  if (done_contains(order_id))
    return;
  if (s_done_count >= DONE_MAX) {
    /* Drop oldest (shift) to make room — bounded memory. */
    memmove(&s_done[0], &s_done[1], sizeof(done_token_t) * (DONE_MAX - 1));
    s_done_count = DONE_MAX - 1;
  }
  strncpy(s_done[s_done_count].order_id, order_id, ORDERID_MAXLEN - 1);
  s_done[s_done_count].order_id[ORDERID_MAXLEN - 1] = '\0';
  s_done_count++;
}

/* ================================================================== */
/* Offline event queue (NVS ring buffer)                               */
/* ================================================================== */
static void q_get_indices(nvs_handle_t h, uint32_t *head, uint32_t *tail) {
  *head = 0;
  *tail = 0;
  nvs_get_u32(h, OFFLINE_KEY_HEAD, head);
  nvs_get_u32(h, OFFLINE_KEY_TAIL, tail);
}

/** @brief Persist one DB event to the NVS queue for later upload. */
static void event_enqueue(const offline_event_t *ev) {
  nvs_handle_t h;
  if (nvs_open(OFFLINE_NS, NVS_READWRITE, &h) != ESP_OK) {
    ESP_LOGE(TAG_TD, "event queue: nvs_open failed");
    return;
  }
  uint32_t head, tail;
  q_get_indices(h, &head, &tail);

  if (tail - head >= OFFLINE_QUEUE_MAX) {
    char old_key[16];
    snprintf(old_key, sizeof(old_key), "e%lu",
             (unsigned long)(head % OFFLINE_QUEUE_MAX));
    nvs_erase_key(h, old_key);
    head++;
    ESP_LOGW(TAG_TD, "event queue full; dropped oldest");
  }

  char key[16];
  snprintf(key, sizeof(key), "e%lu", (unsigned long)(tail % OFFLINE_QUEUE_MAX));
  if (nvs_set_blob(h, key, ev, sizeof(*ev)) == ESP_OK) {
    tail++;
    nvs_set_u32(h, OFFLINE_KEY_HEAD, head);
    nvs_set_u32(h, OFFLINE_KEY_TAIL, tail);
    nvs_commit(h);
    ESP_LOGW(TAG_TD, "queued event %s/%s (pending=%lu)", ev->order_id,
             ev->status, (unsigned long)(tail - head));
  } else {
    ESP_LOGE(TAG_TD, "event queue: set_blob failed");
  }
  nvs_close(h);
}

/** @brief How many events are pending upload. */
static uint32_t event_queue_pending(void) {
  nvs_handle_t h;
  if (nvs_open(OFFLINE_NS, NVS_READONLY, &h) != ESP_OK)
    return 0;
  uint32_t head, tail;
  q_get_indices(h, &head, &tail);
  nvs_close(h);
  return tail - head;
}

/**
 * @brief Send one event to the DB; queue it for retry on failure.
 * @return ESP_OK if delivered, ESP_FAIL if queued for later.
 */
static esp_err_t event_send_or_queue(const offline_event_t *ev) {
  if (supabase_is_online()) {
    if (post_token_event(ev->order_id, ev->token, ev->order_type, ev->status,
                         ev->event_date) == ESP_OK) {
      return ESP_OK;
    }
  }
  event_enqueue(ev);
  return ESP_FAIL;
}

/** @brief Drain queued events to the DB. Returns number flushed. */
static int event_flush(void) {
  int flushed = 0;
  while (supabase_is_online()) {
    nvs_handle_t h;
    if (nvs_open(OFFLINE_NS, NVS_READWRITE, &h) != ESP_OK)
      break;
    uint32_t head, tail;
    q_get_indices(h, &head, &tail);
    if (head >= tail) {
      nvs_close(h);
      break;
    }

    char key[16];
    snprintf(key, sizeof(key), "e%lu",
             (unsigned long)(head % OFFLINE_QUEUE_MAX));
    offline_event_t ev;
    size_t len = sizeof(ev);
    if (nvs_get_blob(h, key, &ev, &len) != ESP_OK) {
      head++;
      nvs_set_u32(h, OFFLINE_KEY_HEAD, head);
      nvs_commit(h);
      nvs_close(h);
      continue;
    }
    nvs_close(h);

    if (post_token_event(ev.order_id, ev.token, ev.order_type, ev.status,
                         ev.event_date) != ESP_OK) {
      break; /* offline again; leave queued */
    }

    if (nvs_open(OFFLINE_NS, NVS_READWRITE, &h) == ESP_OK) {
      nvs_erase_key(h, key);
      head++;
      nvs_set_u32(h, OFFLINE_KEY_HEAD, head);
      nvs_commit(h);
      nvs_close(h);
    }
    flushed++;
  }
  if (flushed > 0)
    ESP_LOGI(TAG_TD, "event queue: flushed %d", flushed);
  return flushed;
}

/* ------------------------------------------------------------------ */
/* Background maintenance: flush-on-online + 6h session wipe           */
/* ------------------------------------------------------------------ */
static void maintenance_task(void *arg) {
  uint64_t last_wipe_us = esp_timer_get_time();
  while (1) {
    if (supabase_is_online()) {
      event_flush();
    }

    /* 6-hour session wipe: clear local lifecycle state + any stale queue. */
    uint64_t now = esp_timer_get_time();
    if (now - last_wipe_us >= SESSION_WIPE_US) {
      xSemaphoreTake(s_state_lock, portMAX_DELAY);
      ESP_LOGW(TAG_TD, "6h session wipe: clearing active/done state");
      state_clear_locked();
      xSemaphoreGive(s_state_lock);
      last_wipe_us = now;
    }
    vTaskDelay(pdMS_TO_TICKS(OFFLINE_FLUSH_MS));
  }
}

/* ------------------------------------------------------------------ */
/* USB Host daemon                                                     */
/* ------------------------------------------------------------------ */
static void usb_lib_task(void *arg) {
  const usb_host_config_t host_config = {
      .skip_phy_setup = false,
      .intr_flags = ESP_INTR_FLAG_LEVEL1,
  };
  ESP_ERROR_CHECK(usb_host_install(&host_config));
  ESP_LOGI(TAG_TD, "USB Host library installed");
  xTaskNotifyGive((TaskHandle_t)arg);
  while (1) {
    uint32_t event_flags;
    usb_host_lib_handle_events(portMAX_DELAY, &event_flags);
    if (event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
      usb_host_device_free_all();
    }
  }
}

int token_display_usb_host_setup(void) {
  TaskHandle_t cur = xTaskGetCurrentTaskHandle();
  if (xTaskCreatePinnedToCore(usb_lib_task, "usb_lib", 4096, (void *)cur, 2,
                              NULL, 0) != pdPASS) {
    ESP_LOGE(TAG_TD, "Failed to create usb_lib task");
    return ESP_FAIL;
  }
  ulTaskNotifyTake(pdFALSE, pdMS_TO_TICKS(5000));
  return ESP_OK;
}

/* ================================================================== */
/* Scan handling — the core lifecycle                                  */
/* ================================================================== */

/**
 * @brief Build an ESP-NOW scan message from a completed event + parsed barcode,
 *        then best-effort send it to the gateway. Independent of Supabase.
 *
 * version/device_mac/seq are stamped inside espnow_send_scan(); we fill the
 * payload. No-op at runtime if ESP-NOW is disabled or not yet initialized.
 */
static void mirror_scan_to_gateway(const offline_event_t *ev,
                                   const parsed_barcode_t *pb) {
  espnow_scan_msg_t msg;
  memset(&msg, 0, sizeof(msg));

  msg.order_type = (strcmp(ev->order_type, "online") == 0)
                       ? ESPNOW_ORDER_ONLINE
                       : ESPNOW_ORDER_OFFLINE;

  if (strcmp(ev->status, "no_show") == 0) {
    msg.status = ESPNOW_STATUS_NO_SHOW;
  } else if (strcmp(ev->status, "packing_complete") == 0) {
    msg.status = ESPNOW_STATUS_PACKING_COMPLETE;
  } else {
    msg.status = ESPNOW_STATUS_READY_TO_COLLECT;
  }

  msg.display_num = pb->is_online ? -1 : pb->display_num;
  strncpy(msg.token, ev->token, sizeof(msg.token) - 1);
  strncpy(msg.order_id, ev->order_id, sizeof(msg.order_id) - 1);
  strncpy(msg.event_date, ev->event_date, sizeof(msg.event_date) - 1);

  espnow_send_scan(&msg);
}

static void handle_scan(const char *raw) {
  parsed_barcode_t pb = parse_barcode(raw);

  if (!pb.valid) {
#if BARCODE_STRICT_FORMAT
    /* Strict mode: reject anything that isn't BY-YYYYMMDD-T<token>. */
    ESP_LOGW(TAG_TD, "ignored: bad barcode format '%s'", raw);
    return;
#else
    /* Relaxed mode: accept any scan as an offline/walk-in token using its
     * last-4 digits. Pure-junk scans with no digits are still ignored. */
    int v = parse_last4(raw);
    if (v < 0) {
      ESP_LOGW(TAG_TD, "ignored: no digits in '%s'", raw);
      return;
    }
    memset(&pb, 0, sizeof(pb));
    pb.valid = true;
    pb.is_online = false;
    pb.display_num = v;
    snprintf(pb.token, sizeof(pb.token), "%d", v);
    pb.date_iso[0] = '\0'; /* unknown date in relaxed mode */
    ESP_LOGW(TAG_TD, "relaxed: '%s' -> offline token %d", raw, v);
#endif
  }

  /* Validate the barcode date == today, when we know today's date. If time
   * is not yet synced (offline boot) or the date is unknown (relaxed mode),
   * skip the check. */
  char today[12];
  today_iso(today, sizeof(today));
  if (pb.date_iso[0] && today[0] && strcmp(today, pb.date_iso) != 0) {
    ESP_LOGW(TAG_TD, "ignored: date %s != today %s ('%s')", pb.date_iso, today,
             raw);
    return;
  }

  offline_event_t ev;
  memset(&ev, 0, sizeof(ev));
  strncpy(ev.order_id, raw, sizeof(ev.order_id) - 1);
  strncpy(ev.token, pb.token, sizeof(ev.token) - 1);
  strncpy(ev.event_date, pb.date_iso, sizeof(ev.event_date) - 1);

  /* -------- ONLINE order: single event, never displayed. -------- */
  if (pb.is_online) {
    strncpy(ev.order_type, "online", sizeof(ev.order_type) - 1);
    strncpy(ev.status, "packing_complete", sizeof(ev.status) - 1);
    ESP_LOGI(TAG_TD, "ONLINE %s -> packing_complete", ev.token);
    event_send_or_queue(&ev);
    mirror_scan_to_gateway(&ev, &pb);
    /* This is the "packed" moment for an online order: one scan, no
     * ready/no-show toggle. Online tokens ARE real, scannable barcodes
     * (BY-YYYYMMDD-TORD<n>, e.g. TORD1) — an earlier assumption that this
     * format never reaches production was wrong (migration 089's own
     * comments document the correction); scan_mark_order_packed() now
     * accepts any alphanumeric token, so this call belongs here too. */
    mark_order_packed(raw);
    return;
  }

  /* -------- OFFLINE / walk-in order: toggle lifecycle. -------- */
  strncpy(ev.order_type, "offline", sizeof(ev.order_type) - 1);

  xSemaphoreTake(s_state_lock, portMAX_DELAY);

  if (done_contains(raw)) {
    /* Already no-show'd this session: permanently done, ignore. */
    xSemaphoreGive(s_state_lock);
    ESP_LOGW(TAG_TD, "offline %s already done (no-show); ignoring", pb.token);
    return;
  }

  int idx = active_find(raw);
  if (idx < 0) {
    /* FIRST scan: ready to collect -> show on display. */
    if (active_add(raw, pb.display_num)) {
      state_save();
      xSemaphoreGive(s_state_lock);
      strncpy(ev.status, "ready_to_collect", sizeof(ev.status) - 1);
      ESP_LOGI(TAG_TD, "offline %s 1st scan -> ready_to_collect (shown)",
               pb.token);
      display_show_now(pb.display_num); /* show this number instantly */
      event_send_or_queue(&ev);
      mirror_scan_to_gateway(&ev, &pb);
      /* This is the "packed" moment: first scan of a walk-in barcode means
       * staff just physically packed the order. Not fired again on the
       * second/no-show scan below (already packed by then — a repeat call
       * would just be harmlessly rejected server-side, so there's no
       * reason to make it). Synchronous, best-effort: no local retry/queue
       * for this one call, unlike event_send_or_queue's NVS-backed queue
       * above, since that queue exists for the physical display-board
       * feature's own consistency needs, not this one. */
      mark_order_packed(raw);
    } else {
      xSemaphoreGive(s_state_lock); /* active set full */
    }
  } else {
    /* SECOND scan: no-show -> remove from display + mark done. */
    active_remove_at(idx);
    done_add(raw);
    state_save();
    xSemaphoreGive(s_state_lock);
    strncpy(ev.status, "no_show", sizeof(ev.status) - 1);
    ESP_LOGI(TAG_TD, "offline %s 2nd scan -> no_show (removed)", pb.token);
    event_send_or_queue(&ev);
    mirror_scan_to_gateway(&ev, &pb);
  }
}

static void scan_handler_task(void *arg) {
  barcode_scanner_event_t bev;
  while (1) {
    if (xQueueReceive(s_scan_queue, &bev, portMAX_DELAY) == pdTRUE) {
      ESP_LOGI(TAG_TD, "Scan: '%s'", bev.data);
      handle_scan(bev.data);
    }
  }
}

/* ------------------------------------------------------------------ */
/* Display: cycle through all currently-active offline tokens          */
/* ------------------------------------------------------------------ */
static void display_task(void *arg) {
  p10_clear();
  p10_draw_text(1, 4, "Hello");
  vTaskDelay(pdMS_TO_TICKS(2000));

  int idx = 0;
  while (1) {
    /* A freshly scanned token jumps the queue: show it instantly, then
     * fall through to resume the normal round-robin from the next slot. */
    int prio = s_priority_num;
    if (prio >= 0) {
      s_priority_num = -1;
      p10_draw_number(prio);
    } else {
      int value = -1;
      int count;

      xSemaphoreTake(s_state_lock, portMAX_DELAY);
      count = s_active_count;
      if (count > 0) {
        if (idx >= count)
          idx = 0;
        value = s_active[idx].display_num;
      }
      xSemaphoreGive(s_state_lock);

      if (count == 0) {
        p10_clear();
      } else {
        p10_draw_number(value);
        idx = (idx + 1) % count;
      }
    }

    /* Sleep until the next cycle tick OR until a new scan wakes us early.
     * ulTaskNotifyTake returns immediately (non-zero) when a notification
     * is pending, so a scan shows up with no perceptible delay. */
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(DISPLAY_CYCLE_MS));
  }
}

/** @brief Request the display to show `num` immediately (called on a new scan).
 */
static void display_show_now(int num) {
  s_priority_num = num;
  if (s_display_task) {
    xTaskNotifyGive(
        s_display_task); /* wake the display task out of its delay */
  }
}

/* ================================================================== */
/* Public entry point                                                  */
/* ================================================================== */
void token_display_start(void) {
  s_state_lock = xSemaphoreCreateMutex();
  if (!s_state_lock) {
    ESP_LOGE(TAG_TD, "Failed to create state mutex");
    return;
  }

  /* Restore active/done sets so the display continues after a reboot. */
  state_load();

  /* P10 display: explicit config (default pins avoid USB D+/D- on GPIO19/20).
   */
  p10_config_t cfg = P10_CONFIG_DEFAULT();
  p10_init_config(&cfg);

  uint8_t brightness = 75;
  nvs_handle_t nvsh;
  if (nvs_open("storage", NVS_READONLY, &nvsh) == ESP_OK) {
    uint32_t val = 75;
    if (nvs_get_u32(nvsh, "brightness", &val) == ESP_OK) {
      brightness = (uint8_t)val;
    }
    nvs_close(nvsh);
  }
  p10_set_brightness_percent(brightness);
  p10_clear();

  /* USB Host + barcode scanner. */
  if (token_display_usb_host_setup() != ESP_OK) {
    ESP_LOGE(TAG_TD, "USB host setup failed; barcode scanner disabled");
  }

  s_scan_queue = xQueueCreate(8, sizeof(barcode_scanner_event_t));
  if (!s_scan_queue) {
    ESP_LOGE(TAG_TD, "Failed to create scan queue");
    return;
  }

  barcode_scanner_config_t bc = BARCODE_SCANNER_DEFAULT_CONFIG();
  bc.out_queue = s_scan_queue;
  bc.terminator = '\r';
  bc.task_core_id = 0;
  esp_err_t err = barcode_scanner_init(&bc);
  if (err != ESP_OK) {
    ESP_LOGE(TAG_TD, "barcode_scanner_init failed: %s", esp_err_to_name(err));
  }

  /* Tier-1 self-test input: the core peripherals (P10 + USB-HID scanner) came
   * up. If the barcode scanner init failed, the device can't do its job, so a
   * freshly-OTA'd image should NOT validate (-> rollback). */
  ota_selftest_set_peripherals_ok(err == ESP_OK);

  xTaskCreate(scan_handler_task, "scan_handler", 8192, NULL, 4, NULL);
  xTaskCreate(display_task, "display_cycle", 3072, NULL, 3, &s_display_task);
  xTaskCreate(maintenance_task, "evt_maint", 8192, NULL, 3, NULL);

  ESP_LOGI(TAG_TD, "token_display started (%lu events pending upload)",
           (unsigned long)event_queue_pending());
}

void token_display_set_brightness(uint8_t percent) {
  if (percent < 1) percent = 1;
  if (percent > 100) percent = 100;
  
  ESP_LOGI(TAG_TD, "Applying P10 display brightness: %d%%", percent);
  p10_set_brightness_percent(percent);

  nvs_handle_t h;
  if (nvs_open("storage", NVS_READWRITE, &h) == ESP_OK) {
    nvs_set_u32(h, "brightness", percent);
    nvs_commit(h);
    nvs_close(h);
  }
}

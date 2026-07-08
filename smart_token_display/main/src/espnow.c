#include "espnow.h"
#include "main.h"

#include <string.h>
#include <stdio.h>
#include <stdbool.h>

#include "esp_log.h"
#include "esp_mac.h"

#define TAG_NOW "ESPNOW"

/* Gateway MAC injected from .env via load_env.cmake (CFG_GATEWAY_MAC). Falls
 * back to broadcast (FF:FF:FF:FF:FF:FF) so the feature still works for testing
 * before the real 7th-ESP MAC is known. Format: "AA:BB:CC:DD:EE:FF". */
#ifdef CFG_GATEWAY_MAC
#define GATEWAY_MAC_STR CFG_GATEWAY_MAC
#else
#define GATEWAY_MAC_STR "FF:FF:FF:FF:FF:FF"
#endif

#if ENABLE_ESPNOW

#include "esp_now.h"
#include "esp_wifi.h"

static bool     s_ready = false;
static uint32_t s_seq   = 0;
static uint8_t  s_gateway_mac[6];

/** @brief Parse "AA:BB:CC:DD:EE:FF" into out[6]; broadcast fallback on failure. */
static void parse_gateway_mac(uint8_t out[6]) {
    unsigned int b[6];
    if (sscanf(GATEWAY_MAC_STR, "%x:%x:%x:%x:%x:%x",
               &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) == 6) {
        for (int i = 0; i < 6; i++) {
            out[i] = (uint8_t)b[i];
        }
    } else {
        ESP_LOGW(TAG_NOW, "bad GATEWAY_MAC '%s'; using broadcast", GATEWAY_MAC_STR);
        memset(out, 0xFF, 6);
    }
}

/** @brief Send callback: log MAC-layer ACK result only (best-effort). */
static void on_send(const uint8_t *mac, esp_now_send_status_t status) {
  (void)mac;
  ESP_LOGD(TAG_NOW, "send cb: status=%d", status);
}

esp_err_t espnow_init(void) {
  if (s_ready) {
    return ESP_OK; /* idempotent */
  }

  esp_err_t err = esp_now_init();
  if (err != ESP_OK) {
    ESP_LOGE(TAG_NOW, "esp_now_init failed: %s", esp_err_to_name(err));
    return err;
  }

  esp_now_register_send_cb(on_send);

  parse_gateway_mac(s_gateway_mac);

  esp_now_peer_info_t peer = {0};
  memcpy(peer.peer_addr, s_gateway_mac, 6);
  peer.channel = 0;     /* 0 = use current STA channel */
  peer.ifidx = WIFI_IF_STA;
  peer.encrypt = false; /* plaintext for now */

  err = esp_now_add_peer(&peer);
  if (err != ESP_OK && err != ESP_ERR_ESPNOW_EXIST) {
    ESP_LOGE(TAG_NOW, "esp_now_add_peer failed: %s", esp_err_to_name(err));
    return err;
  }

  s_ready = true;
  ESP_LOGI(TAG_NOW, "ready: gateway=%02X:%02X:%02X:%02X:%02X:%02X channel=%d",
           s_gateway_mac[0], s_gateway_mac[1], s_gateway_mac[2],
           s_gateway_mac[3], s_gateway_mac[4], s_gateway_mac[5],
           ESPNOW_CHANNEL);
  return ESP_OK;
}

void espnow_send_scan(espnow_scan_msg_t *msg) {
  if (!s_ready || msg == NULL) {
    return; /* safe no-op before init / null */
  }

  /* Stamp the fields the module owns. */
  msg->version = ESPNOW_PROTO_VERSION;
  msg->seq = ++s_seq;
  esp_read_mac(msg->device_mac, ESP_MAC_WIFI_STA);

  wifi_ap_record_t ap_info;
  bool is_connected = (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK);

  if (is_connected) {
    // Online: send on the current channel only
    esp_err_t err =
        esp_now_send(s_gateway_mac, (const uint8_t *)msg, sizeof(*msg));
    if (err != ESP_OK) {
      ESP_LOGW(TAG_NOW, "esp_now_send (connected): %s", esp_err_to_name(err));
    }
  } else {
    // Offline: cycle channels 1 to 11 to ensure the receiver gets it regardless of its channel
    uint8_t orig_ch = 1;
    wifi_second_chan_t orig_second = WIFI_SECOND_CHAN_NONE;
    esp_wifi_get_channel(&orig_ch, &orig_second);

    ESP_LOGI(TAG_NOW,
             "Offline: cycling channels 1..11 to reach receiver...");
    for (uint8_t ch = 1; ch <= 11; ch++) {
      esp_err_t err = esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
      if (err == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(15)); // Wait for radio to lock
        esp_now_send(s_gateway_mac, (const uint8_t *)msg, sizeof(*msg));
        vTaskDelay(pdMS_TO_TICKS(10)); // Give radio time to transmit
      }
    }

    // Restore original channel
    esp_wifi_set_channel(orig_ch, WIFI_SECOND_CHAN_NONE);
    ESP_LOGI(TAG_NOW, "Scan broadcast complete, restored to channel %d", orig_ch);
  }
}


void espnow_repin_channel(void) {
    if (!s_ready) {
        return;
    }
    esp_err_t err = esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);
    if (err != ESP_OK) {
        ESP_LOGW(TAG_NOW, "repin channel %d failed: %s",
                 ESPNOW_CHANNEL, esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG_NOW, "re-pinned radio to channel %d (router down?)", ESPNOW_CHANNEL);
    }
}

#else /* ENABLE_ESPNOW == 0 : compile out, provide no-op stubs */

esp_err_t espnow_init(void) {
    ESP_LOGI(TAG_NOW, "ESP-NOW disabled (ENABLE_ESPNOW=0)");
    return ESP_OK;
}

void espnow_send_scan(espnow_scan_msg_t *msg) {
    (void)msg;
}

void espnow_repin_channel(void) {
}

#endif /* ENABLE_ESPNOW */

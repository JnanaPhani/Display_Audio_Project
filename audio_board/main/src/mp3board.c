#include "mp3board.h"
#include "dfplayer.h"
#include "main.h"
#include "ota_selftest.h"
#include "token_display.h"

#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "mp3board";

/* Wiring for this board (ESP32-S3 DevKit). Override if your physical wiring
 * uses different pins. */
#define MP3_UART_PORT UART_NUM_2
#define MP3_TX_GPIO 7 /* ESP TX -> DFPlayer RX (GPIO 17) */
#define MP3_RX_GPIO 6 /* ESP RX <- DFPlayer TX (GPIO 18) */

#define PLAYBACK_QUEUE_LEN 16

static dfplayer_handle_t s_df = NULL; /* the single DFPlayer instance */
static QueueHandle_t s_play_queue = NULL;

/**
 * @brief Helper to block and wait until a track finishes playing.
 *
 * Polls events from the DFPlayer driver. Returns when DFPLAYER_EVT_FINISHED_TF
 * is received, or if an error/timeout is encountered.
 *
 * @param timeout_ms Max time to wait for the track.
 * @return ESP_OK if completed, ESP_ERR_TIMEOUT on timeout, ESP_FAIL on error.
 */
static esp_err_t wait_for_track_finished(uint32_t timeout_ms) {
  if (s_df == NULL) {
    return ESP_FAIL;
  }

  uint32_t start_ticks = xTaskGetTickCount();
  while (1) {
    dfplayer_event_t evt;
    uint32_t elapsed = (xTaskGetTickCount() - start_ticks) * portTICK_PERIOD_MS;
    if (elapsed >= timeout_ms) {
      ESP_LOGW(TAG, "Timeout waiting for track to finish");
      return ESP_ERR_TIMEOUT;
    }

    uint32_t remaining = timeout_ms - elapsed;
    esp_err_t err = dfplayer_poll_event(s_df, &evt, remaining);
    if (err == ESP_OK) {
      if (evt.cmd == DFPLAYER_EVT_FINISHED_TF) {
        ESP_LOGI(TAG, "Track completed: finished code 0x%02X, param %u",
                 evt.cmd, evt.param);
        return ESP_OK;
      } else if (evt.cmd == DFPLAYER_EVT_ERROR) {
        ESP_LOGE(TAG, "DFPlayer reported play error: %s (0x%02X)",
                 dfplayer_error_name(evt.param & 0xFF), evt.param);
        return ESP_FAIL;
      } else {
        // Process other events but continue waiting
        ESP_LOGD(TAG, "Received other DFPlayer event: 0x%02X", evt.cmd);
      }
    } else if (err == ESP_ERR_TIMEOUT) {
      ESP_LOGW(TAG, "Timeout waiting for event poll");
      return ESP_ERR_TIMEOUT;
    }
  }
}

/**
 * @brief FreeRTOS task to process playback requests sequentially.
 */
static void mp3_playback_task(void *arg) {
  (void)arg;
  int32_t display_num;

  ESP_LOGI(TAG, "Sequential playback task started");

  while (1) {
    if (xQueueReceive(s_play_queue, &display_num, portMAX_DELAY) == pdTRUE) {
      if (display_num < 0) {
        continue; // Ignore invalid/online tokens
      }

      ESP_LOGI(TAG, "Starting announcement for token %ld", (long)display_num);

      // --- 1. Play Prefix ("Token number"): Folder 1, File 1 ---
      ESP_LOGI(TAG, "Playing prefix (Folder 1, File 1)");
      esp_err_t err = dfplayer_play_folder(s_df, 1, 1);
      if (err == ESP_OK) {
        wait_for_track_finished(5000);
      } else {
        ESP_LOGE(TAG, "Failed to play prefix: %s", esp_err_to_name(err));
      }
      vTaskDelay(pdMS_TO_TICKS(100)); // Short gap

      // --- 2. Play Token Number ---
      if (display_num >= 1 && display_num <= 99) {
        // Play number directly: Folder 2, File <num>
        ESP_LOGI(TAG, "Playing token number directly (Folder 2, File %ld)",
                 (long)display_num);
        err = dfplayer_play_folder(s_df, 2, (uint8_t)display_num);
        if (err == ESP_OK) {
          wait_for_track_finished(5000);
        } else {
          ESP_LOGE(TAG, "Failed to play token number: %s",
                   esp_err_to_name(err));
        }
      } else {
        // Fallback: Play digits one-by-one
        ESP_LOGI(TAG,
                 "Token number %ld out of direct bounds (1-99); playing digits "
                 "sequentially",
                 (long)display_num);
        if (display_num == 0) {
          // Play "0" from digits folder: Folder 3, File 1 (0.mp3)
          err = dfplayer_play_folder(s_df, 3, 1);
          if (err == ESP_OK) {
            wait_for_track_finished(5000);
          }
        } else {
          int digits[10];
          int num_digits = 0;
          int temp = display_num;
          while (temp > 0 && num_digits < 10) {
            digits[num_digits++] = temp % 10;
            temp /= 10;
          }
          // Play digits left-to-right
          for (int i = num_digits - 1; i >= 0; i--) {
            int digit = digits[i];
            ESP_LOGI(TAG, "Playing digit %d (Folder 3, File %d)", digit,
                     digit + 1);
            err = dfplayer_play_folder(
                s_df, 3, digit + 1); // 0 is File 1, 1 is File 2, etc.
            if (err == ESP_OK) {
              wait_for_track_finished(5000);
            } else {
              ESP_LOGE(TAG, "Failed to play digit %d: %s", digit,
                       esp_err_to_name(err));
            }
            vTaskDelay(pdMS_TO_TICKS(100));
          }
        }
      }
      vTaskDelay(pdMS_TO_TICKS(100));

      // --- 3. Play Suffix ("Please collect your order"): Folder 1, File 2 ---
      ESP_LOGI(TAG, "Playing suffix (Folder 1, File 2)");
      err = dfplayer_play_folder(s_df, 1, 2);
      if (err == ESP_OK) {
        wait_for_track_finished(5000);
      } else {
        ESP_LOGE(TAG, "Failed to play suffix: %s", esp_err_to_name(err));
      }

      // Post-announcement quiet time (e.g. 500ms) before starting the next
      ESP_LOGI(TAG, "Finished announcement for token %ld", (long)display_num);
      vTaskDelay(pdMS_TO_TICKS(500));
    }
  }
}

esp_err_t mp3board_init(void) {
  if (s_df != NULL) {
    return ESP_OK; // Idempotent
  }

  ESP_LOGI(TAG, "Initializing MP3 board...");

  dfplayer_config_t cfg = DFPLAYER_CONFIG_DEFAULT();
  cfg.uart_port = MP3_UART_PORT;
  cfg.tx_gpio = MP3_TX_GPIO;
  cfg.rx_gpio = MP3_RX_GPIO;
  cfg.reset_on_init = true;

  esp_err_t err = dfplayer_create(&cfg, &s_df);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "dfplayer_create failed: %s", esp_err_to_name(err));
    ota_selftest_set_peripherals_ok(false);
    return err;
  }

  // Set initial volume: 20 out of 30
  dfplayer_set_volume(s_df, 20);

  // Create queue to buffer incoming ESP-NOW announcements
  s_play_queue = xQueueCreate(PLAYBACK_QUEUE_LEN, sizeof(int32_t));
  if (s_play_queue == NULL) {
    ESP_LOGE(TAG, "Failed to create playback queue");
    ota_selftest_set_peripherals_ok(false);
    dfplayer_destroy(s_df);
    s_df = NULL;
    return ESP_ERR_NO_MEM;
  }

  // Create background task for processing playback queue
  BaseType_t ret =
      xTaskCreate(mp3_playback_task, "mp3_playback", 4096, NULL, 5, NULL);
  if (ret != pdPASS) {
    ESP_LOGE(TAG, "Failed to create playback task");
    ota_selftest_set_peripherals_ok(false);
    vQueueDelete(s_play_queue);
    s_play_queue = NULL;
    dfplayer_destroy(s_df);
    s_df = NULL;
    return ESP_FAIL;
  }

  ESP_LOGI(TAG, "MP3 board initialized successfully");

  // Report success to OTA self-test
  ota_selftest_set_peripherals_ok(true);
  return ESP_OK;
}

void mp3board_queue_token(int32_t display_num) {
  if (s_play_queue == NULL) {
    ESP_LOGW(TAG, "Playback queue not initialized; drop token %ld",
             (long)display_num);
    return;
  }

  if (xQueueSend(s_play_queue, &display_num, 0) != pdTRUE) {
    ESP_LOGW(TAG, "Playback queue full; announcement for token %ld dropped",
             (long)display_num);
  }
}

void mp3board_set_volume(uint8_t volume) {
  if (s_df == NULL) {
    ESP_LOGW(TAG, "DFPlayer not initialized; volume setting ignored");
    return;
  }

  if (volume > 30) {
    volume = 30;
  }

  ESP_LOGI(TAG, "Setting DFPlayer volume: %d (0..30)", volume);
  dfplayer_set_volume(s_df, volume);
}

void token_display_set_brightness(uint8_t percent) {
  // Map brightness (1..100%) to announcer volume (0..30)
  uint8_t vol = (percent * 30) / 100;
  ESP_LOGI(TAG,
           "Mapping Supabase brightness update %d%% to announcer volume %d",
           percent, vol);
  mp3board_set_volume(vol);
}

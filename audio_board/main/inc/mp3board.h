#ifndef __MP3BOARD_H__
#define __MP3BOARD_H__

#include <stdint.h>
#include "esp_err.h"

/**
 * @brief Initialize the MP3 board.
 *
 * Configures the UART, initializes the DFPlayer Mini driver instance, and starts
 * the sequential playback processing task/queue.
 *
 * @return ESP_OK on success, or an error code.
 */
esp_err_t mp3board_init(void);

/**
 * @brief Queue a token number to be announced.
 *
 * Pushes the token to a queue that is processed sequentially by the playback task.
 *
 * @param display_num The token number to announce (e.g. 42).
 */
void mp3board_queue_token(int32_t display_num);

/**
 * @brief Directly set the volume of the DFPlayer.
 *
 * @param volume Volume level from 0 to 30.
 */
void mp3board_set_volume(uint8_t volume);

#endif /* __MP3BOARD_H__ */

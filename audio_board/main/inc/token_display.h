#ifndef __TOKEN_DISPLAY_H__
#define __TOKEN_DISPLAY_H__

#include <stdint.h>

/**
 * @brief Map brightness updates from Supabase to announcer volume.
 *
 * @param percent Brightness percentage from 1 to 100.
 */
void token_display_set_brightness(uint8_t percent);

#endif /* __TOKEN_DISPLAY_H__ */

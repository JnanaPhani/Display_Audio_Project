# dfplayer

Portable, instance-based ESP-IDF driver for the **DFPlayer Mini** MP3 module
(UART, 9600 8N1, 10-byte frame protocol).

## Features

- Runtime configuration (UART port, TX/RX GPIO, baud) — no compile-time pin
  macros, so the same image runs on any ESP32 variant and multiple modules can
  coexist.
- Dedicated RX task with a resynchronising frame parser.
- Reliable commands: ACK (0x41) wait + retry.
- Queries return the module's data reply value.
- Playback commands report `ESP_ERR_NOT_FOUND` when the module cannot play the
  selected file (async 0x40 error) instead of silently stopping.
- Asynchronous notifications (track finished, media inserted/ejected, init)
  delivered via an event queue.
- Clean `create` / `destroy` lifecycle.

## Install

Copy this directory into your project's `components/` folder, then add the
dependency in the consuming component:

```cmake
idf_component_register(SRCS "app_main.c"
                       INCLUDE_DIRS "."
                       REQUIRES dfplayer)
```

## Usage

```c
#include "dfplayer.h"

dfplayer_config_t cfg = DFPLAYER_CONFIG_DEFAULT();
cfg.uart_port = UART_NUM_2;
cfg.tx_gpio   = 25;   // ESP TX -> module RX (use a ~1k series resistor)
cfg.rx_gpio   = 26;   // ESP RX <- module TX

dfplayer_handle_t df = NULL;
ESP_ERROR_CHECK(dfplayer_create(&cfg, &df));

dfplayer_set_volume(df, 20);

// Deterministic addressing: /01/002.mp3
if (dfplayer_play_folder(df, 1, 2) == ESP_ERR_NOT_FOUND) {
    // file missing / unplayable
}

// Async events
dfplayer_event_t evt;
if (dfplayer_poll_event(df, &evt, 200) == ESP_OK) {
    printf("event: %s param=%u\n", dfplayer_event_name(evt.cmd), evt.param);
}

dfplayer_destroy(df);
```

## SD card / track-index note

`dfplayer_play_track()` (command 0x03) addresses files by **FAT directory
write order**, not by filename. Bulk drag-copying files can produce an
arbitrary order. For predictable selection, organise files into numbered
folders (`/01/001.mp3`, `/01/002.mp3`, ...) and use `dfplayer_play_folder()`,
or copy files one-by-one in order / sort the FAT (e.g. `fatsort`).

## Wiring

| ESP32        | DFPlayer Mini |
|--------------|---------------|
| TX gpio      | RX (pin 2)*   |
| RX gpio      | TX (pin 3)    |
| GND          | GND           |
| 5V (external)| VCC           |

\* A ~1k series resistor on ESP-TX -> module-RX improves noise immunity.
Avoid GPIO16/17 on WROOM/WROVER modules (flash/PSRAM-reserved).

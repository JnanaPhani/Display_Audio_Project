# p10display

Portable ESP-IDF component: a bit-banged driver for a single red **P10 32×16
HUB12 1/4-scan** LED matrix panel on **ESP32 / ESP32-S3**.

- Continuous scan-out from a dedicated refresh task (pinnable to a core).
- Runtime-configurable pins, signal polarity and pulse timing — no source edits.
- 5×7 text font, 7×11 big-digit font (0–9999), and brightness control.

## Install

Drop this `p10display` folder into your project's `components/` directory:

```
your_project/
├─ CMakeLists.txt
├─ main/
└─ components/
   └─ p10display/        <- this folder
```

ESP-IDF discovers it automatically. Then add it to your `main/CMakeLists.txt`
`REQUIRES` (or `PRIV_REQUIRES`) list:

```cmake
idf_component_register(SRCS "your_app.c"
                       INCLUDE_DIRS "."
                       REQUIRES p10display)
```

## Usage

```c
#include "p10display.h"

void app_main(void)
{
    // Default reference pin map:
    //   DATA=21 CLK=18 LAT=5 OE=4 A=12 B=13, active-low, refresh on core 1.
    p10_init();

    // ...or configure your own wiring:
    // p10_config_t cfg = P10_CONFIG_DEFAULT();
    // cfg.pin_data = GPIO_NUM_23;
    // cfg.pin_clk  = GPIO_NUM_22;
    // p10_init_config(&cfg);

    p10_set_brightness_percent(100);   // 1..100, 100 = brightest

    p10_draw_text(0, 0, "HI");         // 5x7 text
    p10_draw_number(1234);             // big digits, centered
}
```

## API

```c
void p10_init(void);                              // default pin map
void p10_init_config(const p10_config_t *cfg);    // custom config

void p10_clear(void);
void p10_set_pixel(int x, int y, bool on);
int  p10_draw_char(int x, int y, char c);         // 5x7 font
int  p10_draw_text(int x, int y, const char *s);
int  p10_draw_big_digit(int x, int y, int digit); // 7x11 font
void p10_draw_number(int value);                  // 0..9999, centered

void p10_set_brightness(uint8_t level);           // 0..255
void p10_set_brightness_percent(uint8_t percent); // 1..100
```

## Required sdkconfig

The refresh task is a tight busy loop on its pinned core that intentionally
starves that core's IDLE task. Disable the matching idle-task watchdog check or
it will trip (for the default core 1):

```
CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU1=n
```

## Notes on brightness

The panel is **rail-current-limited**, not duty-limited. A short ON pulse plus a
long recovery gap keeps the 5V rail at full voltage and looks *brighter*; a long
pulse sags the rail and looks *dimmer*. `P10_CONFIG_DEFAULT()` ships the
confirmed bright + steady "golden standard" timing (`on_us_max=1500`,
`recover_us=2400`, ~38% duty, ~64 Hz). Further brightness needs hardware (a
74HCT245 buffer driving clean 5V logic plus thicker/shorter 5V & GND wiring).

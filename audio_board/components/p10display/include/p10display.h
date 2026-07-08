/*
 * p10display - portable driver for a single red P10 32x16 HUB12 1/4-scan LED
 * matrix panel, bit-banged from an ESP32 / ESP32-S3 with ESP-IDF.
 *
 * This is a self-contained ESP-IDF component. Drop the `p10display` folder into
 * your project's `components/` directory (or add it via the component manager)
 * and call p10_init() once at boot. Everything below the public API is internal.
 *
 * Pins, signal polarity and pulse timing are all configured at runtime through
 * p10_config_t, so the same binary component works on any wiring without
 * editing the source.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Panel geometry. A single red P10 panel is 32x16, HUB12, 1/4 scan. */
#define P10_WIDTH   32
#define P10_HEIGHT  16

/* ---------------------------------------------------------------------------
 * Configuration
 *
 * Pass a p10_config_t to p10_init_config(). Use P10_CONFIG_DEFAULT() to get a
 * sensible baseline you can then override field by field, e.g.:
 *
 *     p10_config_t cfg = P10_CONFIG_DEFAULT();
 *     cfg.pin_data = GPIO_NUM_23;
 *     cfg.pin_clk  = GPIO_NUM_22;
 *     p10_init_config(&cfg);
 *
 * The convenience p10_init() uses the default pin map (matching the original
 * reference wiring) so existing callers keep working unchanged.
 * ------------------------------------------------------------------------- */
typedef struct {
    /* GPIO numbers for the six HUB12 signals. */
    int pin_data;   /* DATA / R   */
    int pin_clk;    /* CLK        */
    int pin_lat;    /* LAT / STB  */
    int pin_oe;     /* OE  / EN   */
    int pin_a;      /* address A  */
    int pin_b;      /* address B  */

    /* Signal polarity. Standard HUB12 panels are active-LOW on both OE and the
     * data line: OE LOW = LEDs on, and shifting a 0 lights a pixel. Set these
     * false only if your panel behaves inverted. */
    bool oe_active_low;
    bool data_active_low;

    /* Per-scan-group pulse timing in microseconds. The group time slice
     * (on_us_max + recover_us) is held constant so the refresh rate is fixed;
     * only the lit/blank split changes with brightness. The default pair is the
     * confirmed bright + steady "golden standard" for the reference panel. */
    uint32_t on_us_max;    /* maximum per-group ON time at full brightness */
    uint32_t recover_us;   /* rail-recovery gap between groups              */

    /* Core to pin the refresh task to (0 or 1). The refresh loop is a tight
     * busy loop; core 1 is recommended so Wi-Fi/app code on core 0 is not
     * disturbed. Requires CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU<core> = n. */
    int refresh_core;
} p10_config_t;

/* Default configuration: reference pin map, standard active-low polarity,
 * golden-standard timing, refresh on core 1. */
#define P10_CONFIG_DEFAULT()           \
    (p10_config_t){                    \
        .pin_data       = 21,          \
        .pin_clk        = 18,          \
        .pin_lat        = 5,           \
        .pin_oe         = 4,           \
        .pin_a          = 12,          \
        .pin_b          = 13,          \
        .oe_active_low  = true,        \
        .data_active_low= true,        \
        .on_us_max      = 1600u,       \
        .recover_us     = 50u,         \
        .refresh_core   = 1,           \
    }

/* ---------------------------------------------------------------------------
 * Lifecycle
 * ------------------------------------------------------------------------- */

/*
 * Start the driver with an explicit configuration. Configures the GPIOs and
 * spawns the background refresh task that continuously scans the panel from the
 * internal framebuffer. Call once at boot. `cfg` is copied internally.
 */
void p10_init_config(const p10_config_t *cfg);

/* Start the driver with the default configuration (reference pin map). */
void p10_init(void);

/* ---------------------------------------------------------------------------
 * Framebuffer drawing
 * ------------------------------------------------------------------------- */

/* Clear the framebuffer (all pixels off). */
void p10_clear(void);

/* Set a single pixel. on = true turns the LED on. Out-of-range is ignored. */
void p10_set_pixel(int x, int y, bool on);

/*
 * Draw a 5x7 character at (x, y) (top-left origin).
 * Returns the x advance (6: 5 px glyph + 1 px spacing).
 */
int p10_draw_char(int x, int y, char c);

/* Draw a null-terminated string starting at (x, y). Returns end x. */
int p10_draw_text(int x, int y, const char *str);

/*
 * Draw one large digit (0-9) with top-left at (x, y). 7 wide x 11 tall.
 * Returns the x advance (8 px: 7 glyph + 1 spacing).
 */
int p10_draw_big_digit(int x, int y, int digit);

/*
 * Render an integer as up to 4 large digits, centered and filling the panel.
 * Values are clamped to 0..9999. Clears the framebuffer first.
 */
void p10_draw_number(int value);

/* ---------------------------------------------------------------------------
 * Brightness
 * ------------------------------------------------------------------------- */

/* Set brightness 0-255 (maps to per-group ON time). 255 = brightest. */
void p10_set_brightness(uint8_t level);

/*
 * Set brightness as a 1..100 percentage where 100 = brightest (the
 * firmware/rail-limited ceiling) and 1 = dimmest. Input is clamped to 1..100.
 */
void p10_set_brightness_percent(uint8_t percent);

#ifdef __cplusplus
}
#endif

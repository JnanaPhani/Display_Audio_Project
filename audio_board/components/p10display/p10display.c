/*
 * p10display - portable P10 32x16 HUB12 1/4-scan LED matrix driver.
 *
 * Self-contained ESP-IDF component: framebuffer, fonts, scan-out refresh task
 * and brightness control. No application or demo code lives here - see the
 * example in the project's main/ directory for usage.
 */
#include "p10display.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_rom_sys.h"      /* esp_rom_delay_us */
#include "soc/gpio_struct.h"  /* GPIO.out_w1ts / out_w1tc for fast bit-bang */

/* Fast single-cycle pin set/clear (all supported pins are GPIO < 32). Far
 * quicker than gpio_set_level(), which shrinks the unlit shift time -> brighter. */
#define FAST_HI(pin) (GPIO.out_w1ts = (1u << (pin)))
#define FAST_LO(pin) (GPIO.out_w1tc = (1u << (pin)))

/* ------------------------------ driver state ------------------------------ */
/* Active configuration, captured at p10_init_config(). */
static p10_config_t s_cfg;

/* Resolved polarity levels (derived from s_cfg at init). */
static int s_oe_on;
static int s_oe_off;

/* Framebuffer: 1 byte per pixel (0 = off, 1 = on). Simple and plenty of RAM. */
static uint8_t s_fb[P10_HEIGHT][P10_WIDTH];

/* Brightness as a blanking fraction applied per scan group (0..255). */
static volatile uint8_t s_brightness;

/* Convert a framebuffer pixel (1 = want lit) to the bit to drive on DATA. */
static inline int pix_bit(int on)
{
    if (s_cfg.data_active_low) {
        return on ? 0 : 1;
    }
    return on ? 1 : 0;
}

/* ------------------------------ 5x7 font ---------------------------------- */
/* Each glyph: 5 columns, low 7 bits = rows (bit0 = top row). ASCII 0x20..0x7E. */
static const uint8_t FONT5x7[][5] = {
    {0x00,0x00,0x00,0x00,0x00}, /* ' ' */
    {0x00,0x00,0x5F,0x00,0x00}, /* ! */
    {0x00,0x07,0x00,0x07,0x00}, /* " */
    {0x14,0x7F,0x14,0x7F,0x14}, /* # */
    {0x24,0x2A,0x7F,0x2A,0x12}, /* $ */
    {0x23,0x13,0x08,0x64,0x62}, /* % */
    {0x36,0x49,0x55,0x22,0x50}, /* & */
    {0x00,0x05,0x03,0x00,0x00}, /* ' */
    {0x00,0x1C,0x22,0x41,0x00}, /* ( */
    {0x00,0x41,0x22,0x1C,0x00}, /* ) */
    {0x14,0x08,0x3E,0x08,0x14}, /* * */
    {0x08,0x08,0x3E,0x08,0x08}, /* + */
    {0x00,0x50,0x30,0x00,0x00}, /* , */
    {0x08,0x08,0x08,0x08,0x08}, /* - */
    {0x00,0x60,0x60,0x00,0x00}, /* . */
    {0x20,0x10,0x08,0x04,0x02}, /* / */
    {0x3E,0x51,0x49,0x45,0x3E}, /* 0 */
    {0x00,0x42,0x7F,0x40,0x00}, /* 1 */
    {0x42,0x61,0x51,0x49,0x46}, /* 2 */
    {0x21,0x41,0x45,0x4B,0x31}, /* 3 */
    {0x18,0x14,0x12,0x7F,0x10}, /* 4 */
    {0x27,0x45,0x45,0x45,0x39}, /* 5 */
    {0x3C,0x4A,0x49,0x49,0x30}, /* 6 */
    {0x01,0x71,0x09,0x05,0x03}, /* 7 */
    {0x36,0x49,0x49,0x49,0x36}, /* 8 */
    {0x06,0x49,0x49,0x29,0x1E}, /* 9 */
    {0x00,0x36,0x36,0x00,0x00}, /* : */
    {0x00,0x56,0x36,0x00,0x00}, /* ; */
    {0x08,0x14,0x22,0x41,0x00}, /* < */
    {0x14,0x14,0x14,0x14,0x14}, /* = */
    {0x00,0x41,0x22,0x14,0x08}, /* > */
    {0x02,0x01,0x51,0x09,0x06}, /* ? */
    {0x32,0x49,0x79,0x41,0x3E}, /* @ */
    {0x7E,0x11,0x11,0x11,0x7E}, /* A */
    {0x7F,0x49,0x49,0x49,0x36}, /* B */
    {0x3E,0x41,0x41,0x41,0x22}, /* C */
    {0x7F,0x41,0x41,0x22,0x1C}, /* D */
    {0x7F,0x49,0x49,0x49,0x41}, /* E */
    {0x7F,0x09,0x09,0x09,0x01}, /* F */
    {0x3E,0x41,0x49,0x49,0x7A}, /* G */
    {0x7F,0x08,0x08,0x08,0x7F}, /* H */
    {0x00,0x41,0x7F,0x41,0x00}, /* I */
    {0x20,0x40,0x41,0x3F,0x01}, /* J */
    {0x7F,0x08,0x14,0x22,0x41}, /* K */
    {0x7F,0x40,0x40,0x40,0x40}, /* L */
    {0x7F,0x02,0x0C,0x02,0x7F}, /* M */
    {0x7F,0x04,0x08,0x10,0x7F}, /* N */
    {0x3E,0x41,0x41,0x41,0x3E}, /* O */
    {0x7F,0x09,0x09,0x09,0x06}, /* P */
    {0x3E,0x41,0x51,0x21,0x5E}, /* Q */
    {0x7F,0x09,0x19,0x29,0x46}, /* R */
    {0x46,0x49,0x49,0x49,0x31}, /* S */
    {0x01,0x01,0x7F,0x01,0x01}, /* T */
    {0x3F,0x40,0x40,0x40,0x3F}, /* U */
    {0x1F,0x20,0x40,0x20,0x1F}, /* V */
    {0x3F,0x40,0x38,0x40,0x3F}, /* W */
    {0x63,0x14,0x08,0x14,0x63}, /* X */
    {0x07,0x08,0x70,0x08,0x07}, /* Y */
    {0x61,0x51,0x49,0x45,0x43}, /* Z */
    {0x00,0x7F,0x41,0x41,0x00}, /* [ */
    {0x02,0x04,0x08,0x10,0x20}, /* backslash */
    {0x00,0x41,0x41,0x7F,0x00}, /* ] */
    {0x04,0x02,0x01,0x02,0x04}, /* ^ */
    {0x40,0x40,0x40,0x40,0x40}, /* _ */
    {0x00,0x01,0x02,0x04,0x00}, /* ` */
    {0x20,0x54,0x54,0x54,0x78}, /* a */
    {0x7F,0x48,0x44,0x44,0x38}, /* b */
    {0x38,0x44,0x44,0x44,0x20}, /* c */
    {0x38,0x44,0x44,0x48,0x7F}, /* d */
    {0x38,0x54,0x54,0x54,0x18}, /* e */
    {0x08,0x7E,0x09,0x01,0x02}, /* f */
    {0x0C,0x52,0x52,0x52,0x3E}, /* g */
    {0x7F,0x08,0x04,0x04,0x78}, /* h */
    {0x00,0x44,0x7D,0x40,0x00}, /* i */
    {0x20,0x40,0x44,0x3D,0x00}, /* j */
    {0x7F,0x10,0x28,0x44,0x00}, /* k */
    {0x00,0x41,0x7F,0x40,0x00}, /* l */
    {0x7C,0x04,0x18,0x04,0x78}, /* m */
    {0x7C,0x08,0x04,0x04,0x78}, /* n */
    {0x38,0x44,0x44,0x44,0x38}, /* o */
    {0x7C,0x14,0x14,0x14,0x08}, /* p */
    {0x08,0x14,0x14,0x18,0x7C}, /* q */
    {0x7C,0x08,0x04,0x04,0x08}, /* r */
    {0x48,0x54,0x54,0x54,0x20}, /* s */
    {0x04,0x3F,0x44,0x40,0x20}, /* t */
    {0x3C,0x40,0x40,0x20,0x7C}, /* u */
    {0x1C,0x20,0x40,0x20,0x1C}, /* v */
    {0x3C,0x40,0x30,0x40,0x3C}, /* w */
    {0x44,0x28,0x10,0x28,0x44}, /* x */
    {0x0C,0x50,0x50,0x50,0x3C}, /* y */
    {0x44,0x64,0x54,0x4C,0x44}, /* z */
    {0x00,0x08,0x36,0x41,0x00}, /* { */
    {0x00,0x00,0x7F,0x00,0x00}, /* | */
    {0x00,0x41,0x36,0x08,0x00}, /* } */
    {0x08,0x04,0x08,0x10,0x08}, /* ~ */
};

/* --------------------------- framebuffer API ------------------------------ */
void p10_clear(void)
{
    memset(s_fb, 0, sizeof(s_fb));
}

void p10_set_pixel(int x, int y, bool on)
{
    if (x < 0 || x >= P10_WIDTH || y < 0 || y >= P10_HEIGHT) {
        return;
    }
    s_fb[y][x] = on ? 1 : 0;
}

int p10_draw_char(int x, int y, char c)
{
    if (c < 0x20 || c > 0x7E) {
        c = '?';
    }
    const uint8_t *glyph = FONT5x7[(uint8_t)c - 0x20];
    for (int col = 0; col < 5; col++) {
        uint8_t bits = glyph[col];
        for (int row = 0; row < 7; row++) {
            if (bits & (1 << row)) {
                p10_set_pixel(x + col, y + row, true);
            }
        }
    }
    return x + 6; /* 5 px glyph + 1 px spacing */
}

int p10_draw_text(int x, int y, const char *str)
{
    while (*str) {
        x = p10_draw_char(x, y, *str++);
    }
    return x;
}

/* ----------------------------- brightness --------------------------------- */
void p10_set_brightness(uint8_t level)
{
    s_brightness = level;
}

/*
 * Set brightness as a 1..100 percentage. Maps linearly onto the internal
 * 0..255 on-time scale, which in turn drives the per-group OE on-time
 * (on_us_max is the brightness ceiling). Input is clamped to 1..100, so
 * 1% = dimmest visible, 100% = the firmware brightness ceiling.
 */
void p10_set_brightness_percent(uint8_t percent)
{
    if (percent < 1)   percent = 1;
    if (percent > 100) percent = 100;

    /* Scale 1..100 -> 0..255 (rounded). 100% -> 255 (full on_us_max). */
    s_brightness = (uint8_t)((percent * 255u + 50u) / 100u);
}

/* ------------------------- large 7x11 digit font -------------------------- */
/* Each digit: 11 rows x 7 columns of '#'/' '. 4 digits: 4*7 + 3 gaps = 31 px. */
#define BIG_W 7
#define BIG_H 11
#define BIG_Y 2   /* top margin to vertically center the digits */
static const char *const BIGDIGITS[10][BIG_H] = {
    /* 0 */ {" ##### ","##   ##","##   ##","##   ##","##   ##","##   ##","##   ##",
             "##   ##","##   ##","##   ##"," ##### "},
    /* 1 */ {"   ##  ","  ###  "," # ##  ","   ##  ","   ##  ","   ##  ","   ##  ",
             "   ##  ","   ##  ","   ##  "," ##### "},
    /* 2 */ {" ##### ","##   ##","     ##","    ## ","   ##  ","  ##   "," ##    ",
             "##     ","##     ","##     ","#######"},
    /* 3 */ {" ##### ","##   ##","     ##","     ##","  #### ","  #### ","     ##",
             "     ##","     ##","##   ##"," ##### "},
    /* 4 */ {"    ## ","   ### ","  #### "," ## ## ","##  ## ","#######","#######",
             "    ## ","    ## ","    ## ","    ## "},
    /* 5 */ {"#######","##     ","##     ","##     ","#####  ","     ##","     ##",
             "     ##","     ##","##   ##"," ##### "},
    /* 6 */ {"  #### "," ##    ","##     ","##     ","#####  ","##   ##","##   ##",
             "##   ##","##   ##","##   ##"," ##### "},
    /* 7 */ {"#######","     ##","    ## ","    ## ","   ##  ","   ##  ","  ##   ",
             "  ##   "," ##    "," ##    "," ##    "},
    /* 8 */ {" ##### ","##   ##","##   ##","##   ##"," ##### "," ##### ","##   ##",
             "##   ##","##   ##"," ##### "," #####  "},
    /* 9 */ {" ##### ","##   ##","##   ##","##   ##","##   ##"," ######","     ##",
             "     ##","     ##","    ## "," ####  "},
};

int p10_draw_big_digit(int x, int y, int digit)
{
    if (digit < 0 || digit > 9) {
        return x + BIG_W + 1;
    }
    for (int row = 0; row < BIG_H; row++) {
        const char *line = BIGDIGITS[digit][row];
        for (int col = 0; col < BIG_W; col++) {
            if (line[col] == '#') {
                p10_set_pixel(x + col, y + row, true);
            }
        }
    }
    return x + BIG_W + 1; /* 7 glyph + 1 spacing */
}

/* Actual ink width of a big digit (rightmost lit column + 1) for proportional
 * spacing. Narrow glyphs like "1" advance less, so visual gaps stay even. */
static int big_digit_ink_width(int digit)
{
    if (digit < 0 || digit > 9) return BIG_W;
    int maxcol = 0;
    for (int row = 0; row < BIG_H; row++) {
        const char *line = BIGDIGITS[digit][row];
        for (int col = 0; col < BIG_W; col++) {
            if (line[col] == '#' && col > maxcol) maxcol = col;
        }
    }
    return maxcol + 1;
}

void p10_draw_number(int value)
{
    if (value < 0) value = 0;
    if (value > 9999) value = 9999;

    p10_clear();

    int digits[4] = {
        (value / 1000) % 10,
        (value / 100) % 10,
        (value / 10) % 10,
        value % 10,
    };

    /* Proportional layout: advance by each digit's real ink width + a uniform
     * 1px gap, then center the whole block on the 32px panel. */
    const int gap = 1;
    int total = 0;
    for (int i = 0; i < 4; i++) {
        total += big_digit_ink_width(digits[i]);
        if (i < 3) total += gap;
    }
    int x = (P10_WIDTH - total) / 2;
    if (x < 0) x = 0;
    for (int i = 0; i < 4; i++) {
        p10_draw_big_digit(x, BIG_Y, digits[i]);
        x += big_digit_ink_width(digits[i]) + gap;
    }
}

/* ----------------------------- low level IO ------------------------------- */
static inline void clk_pulse(void)
{
    FAST_HI(s_cfg.pin_clk);
    FAST_LO(s_cfg.pin_clk);
}

/*
 * Shift out one 8-pixel column-byte for one physical row, MSB-first.
 * Pixel ON drives DATA per polarity; MSB = leftmost column. Uses fast direct
 * register writes so the (unlit) shift completes quickly -> higher duty.
 */
static inline void shift_byte(const uint8_t *row_on, int col0)
{
    for (int i = 0; i < 8; i++) {
        if (pix_bit(row_on[col0 + i])) {
            FAST_HI(s_cfg.pin_data);
        } else {
            FAST_LO(s_cfg.pin_data);
        }
        clk_pulse();
    }
}

/* --------------------------- refresh scan task ---------------------------- */
/*
 * P10 32x16 1/4-scan sequence (per Freetronics DMD reference):
 * For each scan group g (0..3), shift exactly 32 bits = 4 column-bytes, where
 * the 4 bytes interleave the group's rows (g+12, g+8, g+4, g), column-byte by
 * column-byte. Latch ONCE (OE blanked), set address, then enable for an equal
 * time slice. 4 groups = one full 16-row frame.
 */
static void p10_refresh_task(void *arg)
{
    (void)arg;
    while (1) {
        for (int group = 0; group < 4; group++) {
            const uint8_t *r0 = s_fb[group + 0];
            const uint8_t *r1 = s_fb[group + 4];
            const uint8_t *r2 = s_fb[group + 8];
            const uint8_t *r3 = s_fb[group + 12];

            /* Blank before touching anything; shift while blanked. */
            if (s_oe_off) FAST_HI(s_cfg.pin_oe); else FAST_LO(s_cfg.pin_oe);

            /* 4 column-bytes; for each, emit row3,row2,row1,row0 (far first). */
            for (int col0 = 0; col0 < 32; col0 += 8) {
                shift_byte(r3, col0);
                shift_byte(r2, col0);
                shift_byte(r1, col0);
                shift_byte(r0, col0);
            }

            /* Latch the new data (still blanked, address still = previous). */
            FAST_HI(s_cfg.pin_lat);
            FAST_LO(s_cfg.pin_lat);

            /* Point address at THIS group, brief settle while blanked. */
            if (group & 0x1) FAST_HI(s_cfg.pin_a); else FAST_LO(s_cfg.pin_a);
            if (group & 0x2) FAST_HI(s_cfg.pin_b); else FAST_LO(s_cfg.pin_b);
            esp_rom_delay_us(2);

            /* Constant frame-period brightness control: the per-group slice
             * (on + blank) is fixed at on_us_max + recover_us, so the refresh
             * rate stays constant; only the lit/blank split changes. */
            uint8_t  bright = s_brightness;
            uint32_t slice  = s_cfg.on_us_max + s_cfg.recover_us;
            uint32_t on_us  = (uint32_t)bright * s_cfg.on_us_max / 255u;
            if (on_us > slice) on_us = slice;          /* safety clamp */
            if (on_us > 0) {
                if (s_oe_on) FAST_HI(s_cfg.pin_oe); else FAST_LO(s_cfg.pin_oe);
                esp_rom_delay_us(on_us);
            }
            if (s_oe_off) FAST_HI(s_cfg.pin_oe); else FAST_LO(s_cfg.pin_oe);
            esp_rom_delay_us(slice - on_us);           /* remainder blank */
        }
    }
}

/* ------------------------------- init ------------------------------------- */
void p10_init_config(const p10_config_t *cfg)
{
    s_cfg = *cfg;

    s_oe_on  = s_cfg.oe_active_low ? 0 : 1;
    s_oe_off = s_cfg.oe_active_low ? 1 : 0;

    gpio_config_t io = {
        .pin_bit_mask = (1ULL << s_cfg.pin_data) | (1ULL << s_cfg.pin_clk) |
                        (1ULL << s_cfg.pin_lat)  | (1ULL << s_cfg.pin_oe)  |
                        (1ULL << s_cfg.pin_a)    | (1ULL << s_cfg.pin_b),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);

    gpio_set_level(s_cfg.pin_oe, s_oe_off);  /* start blanked */
    gpio_set_level(s_cfg.pin_lat, 0);
    gpio_set_level(s_cfg.pin_clk, 0);
    gpio_set_level(s_cfg.pin_data, 0);
    gpio_set_level(s_cfg.pin_a, 0);
    gpio_set_level(s_cfg.pin_b, 0);

    p10_clear();
    s_brightness = 255;  /* default full brightness until caller overrides */

    /* Pin the refresh task to the configured core. It is a tight busy loop; the
     * core's IDLE task is intentionally starved, so the matching
     * CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU<core> must be disabled. */
    xTaskCreatePinnedToCore(p10_refresh_task, "p10_refresh", 3072,
                            NULL, 1, NULL, s_cfg.refresh_core);
}

void p10_init(void)
{
    p10_config_t cfg = P10_CONFIG_DEFAULT();
    p10_init_config(&cfg);
}

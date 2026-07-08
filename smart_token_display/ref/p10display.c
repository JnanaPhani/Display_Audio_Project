#include "p10display.h"

#include <string.h>

#include <stdio.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/uart.h"   /* runtime brightness input over UART0 (/dev/ttyACM0 CH343 bridge) */
#include "esp_rom_sys.h"   /* esp_rom_delay_us */
#include "soc/gpio_struct.h"  /* GPIO.out_w1ts / out_w1tc for fast bit-bang */

/* Fast single-cycle pin set/clear (all our pins are GPIO < 32). Far quicker
 * than gpio_set_level(), which shrinks the unlit shift time -> brighter. */
#define FAST_HI(pin) (GPIO.out_w1ts = (1u << (pin)))
#define FAST_LO(pin) (GPIO.out_w1tc = (1u << (pin)))

/* ---------------------------------------------------------------------------
 * Pin map (HUB12 -> ESP32-S3 GPIO)
 *   DATA (R)  -> GPIO21
 *   CLK       -> GPIO18
 *   LAT / STB -> GPIO5
 *   OE / EN   -> GPIO4  (active LOW: LOW = LEDs on)
 *   A         -> GPIO12
 *   B         -> GPIO13
 * ------------------------------------------------------------------------- */
#define PIN_DATA  GPIO_NUM_21
#define PIN_CLK   GPIO_NUM_18
#define PIN_LAT   GPIO_NUM_5
#define PIN_OE    GPIO_NUM_4
#define PIN_A     GPIO_NUM_12
#define PIN_B     GPIO_NUM_13

/*
 * OE (Output Enable) polarity. Standard HUB12 is active-LOW (LOW = LEDs on).
 *   P10_OE_ACTIVE_LOW = 1 -> drive LOW to light, HIGH to blank (default).
 * If "all on" looks INVERTED (lit where it should be dark), set this to 0.
 */
#define P10_OE_ACTIVE_LOW 1

#if P10_OE_ACTIVE_LOW
#  define OE_ON  0
#  define OE_OFF 1
#else
#  define OE_ON  1
#  define OE_OFF 0
#endif

/*
 * Pixel data polarity. P10 panels are active-LOW on the data line: shifting a
 * 0 turns an LED ON, a 1 turns it OFF (confirmed on this panel - digits showed
 * as dark-on-lit until inverted). If your panel is the opposite, set this to 0.
 */
#define P10_DATA_ACTIVE_LOW 1

/* Convert a framebuffer pixel (1 = want lit) to the bit to drive on DATA. */
#if P10_DATA_ACTIVE_LOW
#  define PIX_BIT(on) ((on) ? 0 : 1)
#else
#  define PIX_BIT(on) ((on) ? 1 : 0)
#endif

/* Framebuffer: 1 byte per pixel (0 = off, 1 = on). Simple and plenty of RAM. */
static uint8_t s_fb[P10_HEIGHT][P10_WIDTH];

/* Brightness as a blanking fraction applied per scan group. */
static volatile uint8_t s_brightness;

/* Per-group pulse timing, runtime-tunable so we can sweep for the brightness
 * ceiling over serial without reflashing. Init to the GOLDEN STANDARD baseline
 * (confirmed bright + steady on HW): ON=1800us, RECOVER=2400us (~64Hz). */
static volatile uint32_t s_on_us_max   = 1600u;
static volatile uint32_t s_recover_us  = 50u;

// static volatile uint32_t s_on_us_max   = 1000u;
// static volatile uint32_t s_recover_us  = 3800u;

// static volatile uint32_t s_on_us_max   = 1600u;
// static volatile uint32_t s_recover_us  = 2400u;

/* ------------------------------ 5x7 font ---------------------------------- */
/* Each glyph: 5 columns, low 7 bits = rows (bit0 = top row).               */
/* Covers ASCII 0x20..0x7E.                                                  */
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

void p10_set_brightness(uint8_t level)
{
    s_brightness = level;
}

/*
 * Set brightness as a 1..100 percentage. Maps linearly onto the internal
 * 0..255 on-time scale, which in turn drives the per-group OE on-time
 * (s_on_us_max is the rail-limited ceiling). Input is clamped to 1..100, so
 * 1% = dimmest visible, 100% = the firmware brightness ceiling.
 */
void p10_set_brightness_percent(uint8_t percent)
{
    if (percent < 1)   percent = 1;
    if (percent > 100) percent = 100;
    
    /* Scale 1..100 -> 0..255 (rounded). 100% -> 255 (full s_on_us_max). */
    s_brightness = (uint8_t)((percent * 255u + 50u) / 100u);
}

/* ------------------------- large 7x11 digit font -------------------------- */
/* Each digit defined as 11 ROWS x 7 COLUMNS of '#'/' ' for clarity, drawn
 * directly. 7 wide x 11 tall, balanced proportion. 4 digits: 4*7 + 3 gaps = 31
 * wide -> drawn from x=1 (1px margin each side). y=2 top margin (2+11+3=16). */
#define BIG_W 7
#define BIG_H 11
#define BIG_Y 2   /* top margin to vertically center the digits */
static const char *const BIGDIGITS[10][BIG_H] = {
    /* 0 */ {" ##### ","##   ##","##   ##","##   ##","##   ##","##   ##","##   ##",
             "##   ##","##   ##","##   ##"," ##### "},
    /* 1: top flag + centered stem (cols 3-4) + wide base serif (cols 1-5), so
     * its left/right margins match the other digits and all gaps look even. */
    /* 1 */ {"   ##  ","  ###  "," # ##  ","   ##  ","   ##  ","   ##  ","   ##  ",
             "   ##  ","   ##  ","   ##  "," ##### "},
    /* 2 */ {" ##### ","##   ##","     ##","    ## ","   ##  ","  ##   "," ##    ",
             "##     ","##     ","##     ","#######"},
    /* 3 */ {" ##### ","##   ##","     ##","     ##","  #### ","  #### ","     ##",
             "     ##","     ##","##   ##"," ##### "},
    /* 4: 2px right stem (cols 4-5), 2px diagonal stepping down-left, full
     * 2-row crossbar. All rows exactly 7 chars. */
    /* 4 */ {"    ## ","   ### ","  #### "," ## ## ","##  ## ","#######","#######",
             "    ## ","    ## ","    ## ","    ## "},
    /* 5: full top bar, left stem to the waist, then a bowl that closes on the
     * bottom-right. Spans all 11 rows like the other digits. */
    /* 5 */ {"#######","##     ","##     ","##     ","#####  ","     ##","     ##",
             "     ##","     ##","##   ##"," ##### "},
    /* 6: curved-in top, straight left stem, closed lower bowl. */
    /* 6 */ {"  #### "," ##    ","##     ","##     ","#####  ","##   ##","##   ##",
             "##   ##","##   ##","##   ##"," ##### "},
    /* 7 */ {"#######","     ##","    ## ","    ## ","   ##  ","   ##  ","  ##   ",
             "  ##   "," ##    "," ##    "," ##    "},
    /* 8: top-heavy. Tall full-width upper bowl (cols 0-6, 2px sides), pinched
     * waist, and a shorter, narrower lower bowl (1px sides at cols 1 & 5). */
    /* 8 */
        {
        " ##### ",
        "##   ##",
        "##   ##",
        "##   ##",
        " ##### ",
        " ##### ",
        "##   ##",
        "##   ##",
        "##   ##",
        " ##### ",
        " #####  "
        },
    /* 9: closed upper bowl, right stem straight down, tail curving out at the
     * bottom-left (mirror of 6). */
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
    return x + BIG_W + 1; /* 6 glyph + 1 spacing */
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
     * 1px gap, so the visible space between every pair of digits is equal even
     * though "1" is narrower. Then center the whole block on the 32px panel. */
    const int gap = 1;
    int total = 0;
    for (int i = 0; i < 4; i++) {
        total += big_digit_ink_width(digits[i]);
        if (i < 3) total += gap;
    }
    int x = (P10_WIDTH - total) / 2;   /* center horizontally */
    if (x < 0) x = 0;
    for (int i = 0; i < 4; i++) {
        p10_draw_big_digit(x, BIG_Y, digits[i]);
        x += big_digit_ink_width(digits[i]) + gap;
    }
}

/* ----------------------------- low level IO ------------------------------- */
static inline void clk_pulse(void)
{
    FAST_HI(PIN_CLK);
    FAST_LO(PIN_CLK);
}

/*
 * Shift out one 8-pixel column-byte for one physical row, MSB-first.
 * pixel ON drives DATA per polarity; MSB = leftmost column. Uses fast direct
 * register writes so the (unlit) shift completes quickly -> higher duty.
 */
static inline void shift_byte(const uint8_t *row_on, int col0)
{
    for (int i = 0; i < 8; i++) {
        if (PIX_BIT(row_on[col0 + i])) {
            FAST_HI(PIN_DATA);
        } else {
            FAST_LO(PIN_DATA);
        }
        clk_pulse();
    }
}

/* --------------------------- refresh scan task ---------------------------- */
/*
 * Correct P10 32x16 1/4-scan sequence (per Freetronics DMD reference):
 * For each scan group g (0..3), shift exactly 32 bits = 4 column-bytes, where
 * the 4 bytes interleave the 4 rows of the group as rows (g+12, g+8, g+4, g),
 * column-byte by column-byte. Latch ONCE (OE blanked), set address, then
 * enable for an equal time slice. 4 groups = one full 16-row frame.
 */
static void p10_refresh_task(void *arg)
{
    (void)arg;
    /* Core 1 is dedicated to this tight refresh loop; the core-1 IDLE task is
     * intentionally starved. The IDLE1 task-watchdog check is disabled in
     * sdkconfig (CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU1 unset) so this is OK
     * and needs no blanking yield -> flicker-free. */
    while (1) {
        for (int group = 0; group < 4; group++) {
            const uint8_t *r0 = s_fb[group + 0];
            const uint8_t *r1 = s_fb[group + 4];
            const uint8_t *r2 = s_fb[group + 8];
            const uint8_t *r3 = s_fb[group + 12];

            /* Blank (fast) before touching anything; shift while blanked. */
            if (OE_OFF) FAST_HI(PIN_OE); else FAST_LO(PIN_OE);

            /* 4 column-bytes; for each, emit row3,row2,row1,row0 (far first).
             * Fast register writes -> short unlit shift -> higher duty. */
            for (int col0 = 0; col0 < 32; col0 += 8) {
                shift_byte(r3, col0);
                shift_byte(r2, col0);
                shift_byte(r1, col0);
                shift_byte(r0, col0);
            }

            /* Latch the new data (still blanked, address still = previous). */
            FAST_HI(PIN_LAT);
            FAST_LO(PIN_LAT);

            /* Point address at THIS group, brief settle while blanked. */
            if (group & 0x1) FAST_HI(PIN_A); else FAST_LO(PIN_A);
            if (group & 0x2) FAST_HI(PIN_B); else FAST_LO(PIN_B);
            esp_rom_delay_us(2);

            /* Enable for the on-time slice. With the faster shift the lit
             * fraction (duty) is higher at the same peak current, so the panel
             * is BRIGHTER without sagging the rail. P10_ON_US_MAX tunes it. */
            /* Full-voltage-pulse strategy: a moderate ON time followed by a
             * recovery gap that lets the 5V rail rebound before the next
             * high-current group. This keeps each pulse at full LED voltage =
             * brighter AND crisper than one long sagging pulse. Tune the pair:
             * more ON = brighter but sags; more recovery = brighter per pulse
             * but lower refresh (watch for flicker).
             *
             * Timing is now runtime-tunable via s_on_us_max / s_recover_us so we
             * can sweep for the brightness ceiling over serial without
             * reflashing. GOLDEN STANDARD baseline (confirmed bright + steady on
             * HW): ON=1500us, RECOVER=2400us (~64Hz). */
            /* Constant frame-period brightness control: the per-group time
             * slice (on + blank) is fixed at s_on_us_max + s_recover_us, so the
             * refresh rate stays constant (~64 Hz, flicker-free) at every
             * brightness. Only the split between lit and blank changes:
             * brighter = more on_us, less blank; the sum is invariant. This
             * avoids the old behaviour where higher brightness lengthened the
             * frame and dropped refresh below the flicker threshold. */
            uint8_t  bright = s_brightness;
            uint32_t slice  = s_on_us_max + s_recover_us;          /* fixed budget */
            uint32_t on_us  = (uint32_t)bright * s_on_us_max / 255u;
            if (on_us > slice) on_us = slice;                      /* safety clamp */
            if (on_us > 0) {
                if (OE_ON) FAST_HI(PIN_OE); else FAST_LO(PIN_OE);
                esp_rom_delay_us(on_us);
            }
            if (OE_OFF) FAST_HI(PIN_OE); else FAST_LO(PIN_OE);  /* blank */
            esp_rom_delay_us(slice - on_us);                       /* remainder blank */
        }
    }
}

/* ----------------------- runtime brightness input ------------------------ */
/*
 * Reads characters from UART0 (the /dev/ttyACM0 device you type into - it is a
 * CH343 USB-UART bridge wired to the ESP32-S3 UART0 TX/RX pins, which is also
 * the primary console). Accumulates a line and parses a bare integer 1..100,
 * then applies it via p10_set_brightness_percent(). Echoes a confirmation back.
 *
 * NOTE: the secondary USB-Serial-JTAG console is OUTPUT ONLY (per IDF docs) and
 * is NOT the port we type into here, so we read UART0 directly.
 */
#define P10_INPUT_UART UART_NUM_0

static void p10_brightness_input_task(void *arg)
{
    (void)arg;

    /* The primary console already configures UART0's pins/baud; we just need a
     * driver with an RX buffer so we can read typed bytes. install_driver is
     * idempotent-safe to call once here. */
    if (!uart_is_driver_installed(P10_INPUT_UART)) {
        uart_driver_install(P10_INPUT_UART, 256, 0, 0, NULL, 0);
    }

    char line[16];
    int  len = 0;
    uint8_t byte;

    for (;;) {
        int n = uart_read_bytes(P10_INPUT_UART, &byte, 1, pdMS_TO_TICKS(100));
        if (n <= 0) {
            continue;
        }

        if (byte == '\r' || byte == '\n') {
            if (len == 0) {
                continue;  /* ignore empty lines / bare CR-LF */
            }
            line[len] = '\0';
            int percent = atoi(line);
            if (percent >= 1 && percent <= 100) {
                p10_set_brightness_percent((uint8_t)percent);
                printf("[bright] set to %d%%\n", percent);
            } else {
                printf("[bright] ignored '%s' (enter 1..100)\n", line);
            }
            len = 0;
        } else if (byte >= '0' && byte <= '9') {
            if (len < (int)sizeof(line) - 1) {
                line[len++] = (char)byte;
            }
        }
        /* Any other character (spaces, letters) is ignored. */
    }
}

/* ------------------------------- init ------------------------------------- */
void p10_init(void)
{
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << PIN_DATA) | (1ULL << PIN_CLK) |
                        (1ULL << PIN_LAT)  | (1ULL << PIN_OE)  |
                        (1ULL << PIN_A)    | (1ULL << PIN_B),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);

    gpio_set_level(PIN_OE, OE_OFF);  /* start blanked */
    gpio_set_level(PIN_LAT, 0);
    gpio_set_level(PIN_CLK, 0);
    gpio_set_level(PIN_DATA, 0);
    gpio_set_level(PIN_A, 0);
    gpio_set_level(PIN_B, 0);

    p10_clear();

    /* Pin the refresh task to core 1 so it isn't disturbed by app/wifi on
     * core 0. LOW priority (1) so the core-1 IDLE task (0) still gets serviced
     * between frames -> no watchdog trip and no need for a blanking delay that
     * would cause a visible flash. Core 1 is otherwise idle so timing is tight. */
    xTaskCreatePinnedToCore(p10_refresh_task, "p10_refresh", 3072,
                            NULL, 1, NULL, 1);
}

/* -------------------------------- demo ------------------------------------ */
/* Seconds between counter increments. */
#define P10_COUNT_INTERVAL_S 10

void app_main(void)
{
    p10_init();
    p10_set_brightness_percent(100);  /* 1..100% brightness */

    /* Accept live brightness changes over the USB-Serial-JTAG port: type a
     * number 1..100 and press Enter. Pinned to core 0 so it never disturbs the
     * core-1 refresh loop. */
    xTaskCreatePinnedToCore(p10_brightness_input_task, "p10_bright_in", 3072,
                            NULL, 5, NULL, 0);

       /* Brightness push: report the duty/refresh the current timing yields so we
     * can correlate what we see on the panel with the numbers. */
    uint32_t period = 4u * (s_on_us_max + s_recover_us);
    uint32_t hz = period ? (1000000u / period) : 0;
    uint32_t duty = 100u * s_on_us_max / (s_on_us_max + s_recover_us);
    printf("[bright] ON=%lu us  RECOVER=%lu us  duty=%lu%%  ~%lu Hz\n",
           (unsigned long)s_on_us_max, (unsigned long)s_recover_us,
           (unsigned long)duty, (unsigned long)hz);


    /* Count 0000 -> 9999, one increment every P10_COUNT_INTERVAL_S seconds,
     * wrapping back to 0000 after 9999. The refresh task on core 1 keeps the
     * current value on screen between updates. */
    int value = 0;
    while (1) {
        p10_draw_number(value);
        vTaskDelay(pdMS_TO_TICKS(P10_COUNT_INTERVAL_S * 100));
        value = (value + 1) % 10000;
    }
}

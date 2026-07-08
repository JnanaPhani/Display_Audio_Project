/*
 * Example app for the p10display component.
 *
 * Demonstrates how to use the reusable P10 driver:
 *   - start the driver (default reference pin map)
 *   - count 0000 -> 9999 on the panel using the big-digit font
 *   - accept live brightness changes (1..100) typed over the serial console
 *
 * The driver itself lives in components/p10display and has no app-specific
 * code; everything here is example glue you can copy or delete.
 */
#include <stdio.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"

#include "p10display.h"

/* Seconds between counter increments. */
#define COUNT_INTERVAL_S 10

/* The console UART. On this board /dev/ttyACM0 is a CH343 USB-UART bridge wired
 * to UART0 TX/RX, so we read typed brightness values directly from UART0. */
#define INPUT_UART UART_NUM_0

/*
 * Reads characters from the console UART, accumulates a line, and applies a
 * bare integer 1..100 as a brightness percentage. Type a number + Enter.
 */
static void brightness_input_task(void *arg)
{
    (void)arg;

    if (!uart_is_driver_installed(INPUT_UART)) {
        uart_driver_install(INPUT_UART, 256, 0, 0, NULL, 0);
    }

    char line[16];
    int  len = 0;
    uint8_t byte;

    for (;;) {
        int n = uart_read_bytes(INPUT_UART, &byte, 1, pdMS_TO_TICKS(100));
        if (n <= 0) {
            continue;
        }

        if (byte == '\r' || byte == '\n') {
            if (len == 0) {
                continue;
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
        /* Any other character is ignored. */
    }
}

void app_main(void)
{
    p10_init();                        /* default pin map, refresh on core 1 */
    p10_set_brightness_percent(100);

    /* Live brightness over serial; pinned to core 0 so it never disturbs the
     * core-1 refresh loop. */
    xTaskCreatePinnedToCore(brightness_input_task, "p10_bright_in", 3072,
                            NULL, 5, NULL, 0);

    printf("[p10] example running: type 1..100 + Enter to set brightness\n");

    /* Count 0000 -> 9999, wrapping. The refresh task keeps the value on screen
     * between updates. */
    int value = 0;
    while (1) {
        p10_draw_number(value);
        vTaskDelay(pdMS_TO_TICKS(COUNT_INTERVAL_S * 100));
        value = (value + 1) % 10000;
    }
}

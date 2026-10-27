/* lcd1602_i2c.c - HD44780 16x2 LCD behind a PCF8574 I2C backpack.
 * Uses the I2C driver that ds3231_init() installs, so call that first. */
#include "lcd1602_i2c.h"
#include <stdio.h>
#include <stdarg.h>
#include <stdbool.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c.h"

#define LCD_I2C_PORT  I2C_NUM_0
#define BL  0x08   /* backlight   */
#define EN  0x04   /* enable      */
#define RS  0x01   /* data (1) / command (0) */

static uint8_t s_addr;

/* The PCF8574 carries RS/EN/backlight in the low bits and the nibble in the
 * high bits; EN is pulsed high then low to latch the nibble. */
static void write_nibble(uint8_t nibble, bool rs)
{
    uint8_t d  = (uint8_t)((nibble << 4) | (rs ? RS : 0) | BL);
    uint8_t hi = d | EN;
    uint8_t lo = d;

    i2c_master_write_to_device(LCD_I2C_PORT, s_addr, &hi, 1, pdMS_TO_TICKS(100));
    vTaskDelay(pdMS_TO_TICKS(1));
    i2c_master_write_to_device(LCD_I2C_PORT, s_addr, &lo, 1, pdMS_TO_TICKS(100));
    vTaskDelay(pdMS_TO_TICKS(1));
}

static void write_byte(uint8_t val, bool rs)
{
    write_nibble(val >> 4, rs);
    write_nibble(val & 0x0F, rs);
}

esp_err_t lcd_init(uint8_t i2c_addr)
{
    s_addr = i2c_addr;
    vTaskDelay(pdMS_TO_TICKS(50));
    write_nibble(0x03, false); vTaskDelay(pdMS_TO_TICKS(5));
    write_nibble(0x03, false); vTaskDelay(pdMS_TO_TICKS(5));
    write_nibble(0x03, false); vTaskDelay(pdMS_TO_TICKS(1));
    write_nibble(0x02, false); vTaskDelay(pdMS_TO_TICKS(1));   /* 4-bit mode */
    write_byte(0x28, false);   /* 4-bit, 2 lines, 5x8 font */
    write_byte(0x0C, false);   /* display on, cursor off   */
    write_byte(0x06, false);   /* entry mode               */
    write_byte(0x01, false);   /* clear                    */
    vTaskDelay(pdMS_TO_TICKS(3));
    return ESP_OK;
}

/* printf into the LCD at (row, col). The text is padded / cut to the rest of
 * the 16-column line so leftovers of a longer previous text never remain. */
void lcd_printf(uint8_t row, uint8_t col, const char *fmt, ...)
{
    char buf[17];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    size_t width = (col < 16) ? (size_t)(16 - col) : 0;
    size_t len = strlen(buf);
    if (len > width) len = width;
    while (len < width) buf[len++] = ' ';
    buf[len] = '\0';

    write_byte((uint8_t)(0x80 | (row == 1 ? 0x40 : 0x00) | col), false);   /* set cursor */
    for (const char *s = buf; *s; s++) write_byte((uint8_t)*s, true);
}

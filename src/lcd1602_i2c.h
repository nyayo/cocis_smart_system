/* lcd1602_i2c.h - HD44780 16x2 LCD via PCF8574 I2C backpack */
#pragma once
#include <stdint.h>
#include "esp_err.h"

esp_err_t lcd_init(uint8_t i2c_addr);
/* printf at (row, col); the rest of the 16-column line is blank-padded. */
void      lcd_printf(uint8_t row, uint8_t col, const char *fmt, ...);

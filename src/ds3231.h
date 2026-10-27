/* ds3231.h - DS3231 / DS1307 RTC over I2C (with software-clock fallback) */
#pragma once
#include <stdbool.h>
#include <time.h>
#include "esp_err.h"

esp_err_t ds3231_init(void);
esp_err_t ds3231_get_time(struct tm *t);
esp_err_t ds3231_set_time(const struct tm *t);
/* true if a real RTC chip answered at init time, false = software clock */
bool      ds3231_available(void);

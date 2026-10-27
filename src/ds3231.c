/* ds3231.c - DS3231 / DS1307 RTC over I2C (same address 0x68 and time-register
 * layout, so it also works with Wokwi's DS1307 part).
 *
 * If no RTC answers on the bus the driver falls back to the ESP32 software
 * clock instead of aborting; that clock restarts from 1970 at every boot
 * until "settime" / "setdate" are used. */
#include "ds3231.h"
#include <string.h>
#include <sys/time.h>
#include "freertos/FreeRTOS.h"
#include "driver/i2c.h"
#include "esp_log.h"
#include "common.h"

#define DS3231_ADDR  0x68
#define I2C_PORT     I2C_NUM_0

static const char *TAG = "rtc";
static bool s_present = false;

static uint8_t bcd2dec(uint8_t v) { return (uint8_t)((v >> 4) * 10 + (v & 0x0F)); }
static uint8_t dec2bcd(uint8_t v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }

bool ds3231_available(void) { return s_present; }

/* Installs the I2C driver (shared with the LCD) and probes for the RTC. */
esp_err_t ds3231_init(void)
{
    i2c_config_t cfg = {
        .mode             = I2C_MODE_MASTER,
        .sda_io_num       = PIN_I2C_SDA,
        .scl_io_num       = PIN_I2C_SCL,
        .sda_pullup_en    = GPIO_PULLUP_ENABLE,
        .scl_pullup_en    = GPIO_PULLUP_ENABLE,
        .master.clk_speed = 100000,
    };
    ESP_ERROR_CHECK(i2c_param_config(I2C_PORT, &cfg));
    esp_err_t err = i2c_driver_install(I2C_PORT, I2C_MODE_MASTER, 0, 0, 0);
    if (err != ESP_OK) return err;

    uint8_t reg = 0x00, probe = 0;
    s_present = (i2c_master_write_read_device(I2C_PORT, DS3231_ADDR, &reg, 1,
                                              &probe, 1, pdMS_TO_TICKS(100)) == ESP_OK);
    if (s_present) ESP_LOGI(TAG, "RTC found at 0x%02X", DS3231_ADDR);
    else           ESP_LOGW(TAG, "no RTC answered at 0x%02X - using software clock", DS3231_ADDR);
    return ESP_OK;
}

esp_err_t ds3231_get_time(struct tm *t)
{
    if (s_present) {
        uint8_t reg = 0x00, buf[7];
        esp_err_t err = i2c_master_write_read_device(I2C_PORT, DS3231_ADDR, &reg, 1,
                                                     buf, 7, pdMS_TO_TICKS(100));
        if (err == ESP_OK) {
            memset(t, 0, sizeof(*t));
            t->tm_sec  = bcd2dec(buf[0] & 0x7F);
            t->tm_min  = bcd2dec(buf[1]);
            t->tm_hour = bcd2dec(buf[2] & 0x3F);      /* 24h mode */
            t->tm_wday = bcd2dec(buf[3]) - 1;
            t->tm_mday = bcd2dec(buf[4]);
            t->tm_mon  = bcd2dec(buf[5] & 0x1F) - 1;
            t->tm_year = bcd2dec(buf[6]) + 100;       /* years since 1900 */
            return ESP_OK;
        }
        ESP_LOGW(TAG, "RTC read failed (%s) - software clock used for this read",
                 esp_err_to_name(err));
    }
    time_t now = time(NULL);
    localtime_r(&now, t);
    return ESP_OK;
}

esp_err_t ds3231_set_time(const struct tm *t)
{
    if (s_present) {
        uint8_t buf[8] = {
            0x00,                                       /* start register */
            dec2bcd((uint8_t)t->tm_sec),
            dec2bcd((uint8_t)t->tm_min),
            dec2bcd((uint8_t)t->tm_hour),               /* 24h mode */
            dec2bcd((uint8_t)(t->tm_wday + 1)),
            dec2bcd((uint8_t)t->tm_mday),
            dec2bcd((uint8_t)(t->tm_mon + 1)),
            dec2bcd((uint8_t)(t->tm_year - 100)),
        };
        return i2c_master_write_to_device(I2C_PORT, DS3231_ADDR, buf, 8, pdMS_TO_TICKS(100));
    }

    struct tm tmp = *t;   /* software-clock fallback */
    struct timeval tv = { .tv_sec = mktime(&tmp), .tv_usec = 0 };
    return settimeofday(&tv, NULL) == 0 ? ESP_OK : ESP_FAIL;
}

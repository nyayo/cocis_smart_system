/* rc522.h - Minimal MFRC522 (SPI) driver: init + UID read */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "driver/spi_master.h"

typedef struct {
    spi_device_handle_t spi;
    int                 rst_gpio;
} rc522_t;

esp_err_t rc522_init(rc522_t *dev, spi_host_device_t host, int cs_pin, int rst_pin);
/* Returns true and fills uid[4] if a tag is present in the field. */
bool      rc522_read_uid(rc522_t *dev, uint8_t uid[4]);

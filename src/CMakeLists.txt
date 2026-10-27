idf_component_register(
    SRCS
        "main.c"
        "rc522.c"
        "ds3231.c"
        "lcd1602_i2c.c"
        "entrance_main.c"
        "room_main.c"
    INCLUDE_DIRS "."
    REQUIRES nvs_flash esp_wifi esp_event esp_netif driver fatfs sdmmc esp_timer
)

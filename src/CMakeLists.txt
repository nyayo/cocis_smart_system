idf_component_register(
    SRCS "main.c" "rc522.c" "ds3231.c" "lcd1602_i2c.c" "entrance_main.c" "access_control.c" "system_manager.c" "room_main.c"
    INCLUDE_DIRS "."
    REQUIRES nvs_flash driver fatfs sdmmc esp_timer
)

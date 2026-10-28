/* Shared pin map and room status message for one-ESP32 Wokwi project. */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#define PIN_RC522_SCK 18
#define PIN_RC522_MISO 19
#define PIN_RC522_MOSI 23
#define PIN_RC522_IN_CS 15
#define PIN_RC522_OUT_CS 2
#define PIN_RC522_IN_RST 4
#define PIN_RC522_OUT_RST (-1)
#define PIN_SD_SCK 14
#define PIN_SD_MISO 12
#define PIN_SD_MOSI 13
#define PIN_SD_CS 5
#define PIN_I2C_SDA 21
#define PIN_I2C_SCL 22
#define PIN_SERVO_IN 25
#define PIN_SERVO_OUT 26
#define PIN_BUZZER 17
#define PIN_LED_GREEN 32
#define PIN_LED_RED 33
#define PIN_ROOM1_PIR 34
#define PIN_ROOM1_LIGHT 27
#define PIN_ROOM2_PIR 35
#define PIN_ROOM2_LIGHT 16
#define LIGHT_WATTS 40
#define ROOM_EMPTY_TIMEOUT_S 30
#define LOG_FILE_PATH "/sdcard/access.csv"
#define TAGS_FILE_PATH "/sdcard/tags.csv"
#define COCIS_MAGIC 0x51CC05E
#define MSG_ROOM_STATUS 1
typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint8_t msg_type, room_id, occupied, light_on;
    uint16_t motion_events;
    uint32_t uptime_sec;
} room_status_msg_t;
void room_start(int room_id, int pir_pin, int light_pin, int status_pin);
bool room_status_post(const room_status_msg_t *m);

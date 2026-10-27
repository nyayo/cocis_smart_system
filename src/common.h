/*
 * common.h - shared definitions for the CoCIS Smart Control & Monitoring System
 *
 * Two builds (see platformio.ini):
 *   STANDALONE   (env "standalone")              ONE ESP32 runs everything; the
 *                room tasks hand their status to the entrance through a FreeRTOS
 *                queue. This is what Wokwi can simulate.
 *   DISTRIBUTED  (envs "entrance","room1","room2") three ESP32 boards; the rooms
 *                send their status to the entrance over ESP-NOW.
 *
 * In both builds the entrance terminal is the SINGLE WRITER to the SD card, so
 * no file-level races are possible. FreeRTOS mutexes guard the shared state,
 * the SD card, the LCD and the door hardware.
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>

/* ---------------- Entrance terminal pins ---------------- */
#define PIN_RC522_SCK     18
#define PIN_RC522_MISO    19
#define PIN_RC522_MOSI    23
#define PIN_RC522_IN_CS   15
#define PIN_RC522_OUT_CS   2
#define PIN_RC522_IN_RST   4    /* both readers share this reset line        */
#define PIN_RC522_OUT_RST (-1)  /* -1 = do not touch a reset pin             */

/* SD card on a SECOND SPI bus (HSPI) to avoid bus-init conflicts */
#define PIN_SD_SCK   14
#define PIN_SD_MISO  12
#define PIN_SD_MOSI  13
#define PIN_SD_CS     5

#define PIN_I2C_SDA  21
#define PIN_I2C_SCL  22
#define PIN_SERVO_IN  25
#define PIN_SERVO_OUT 26
#define PIN_BUZZER    17
#define PIN_LED_GREEN 32
#define PIN_LED_RED   33

/* ---------------- Room pins: DISTRIBUTED build (one board per room) ---- */
#define PIN_PIR         13
#define PIN_LIGHT       14
#define PIN_STATUS_LED  27

/* ---------------- Room pins: STANDALONE build (both rooms on one board) -
 * GPIO34/35 are input-only pins, which is what a PIR output needs. */
#define PIN_ROOM1_PIR    34
#define PIN_ROOM1_LIGHT  27
#define PIN_ROOM2_PIR    35
#define PIN_ROOM2_LIGHT  16

/* ASSUMED power of one room light - only used for the energy-saving
 * estimate of the "energy" report. Change it to match the real fitting. */
#define LIGHT_WATTS 40

/* A room switches its light off this long after the last motion */
#define ROOM_EMPTY_TIMEOUT_S 30

/* SD card files. The build uses CONFIG_FATFS_LFN_NONE, so FAT only accepts
 * 8.3 file names. */
#define LOG_FILE_PATH   "/sdcard/access.csv"
#define TAGS_FILE_PATH  "/sdcard/tags.csv"   /* one "UID,Name" line per tag */

/* ---------------- ESP-NOW (distributed build) ---------------- */
/* Fixed, locally-administered MAC of the entrance terminal; the room
 * firmware is compiled with it as its ESP-NOW peer. */
#define ENTRANCE_MAC_ADDR  {0x84, 0xF7, 0x03, 0xC0, 0xC1, 0x01}

#define COCIS_MAGIC      0x51CC05E
#define MSG_ROOM_STATUS  1

typedef struct __attribute__((packed)) {
    uint32_t magic;          /* COCIS_MAGIC - filters foreign packets */
    uint8_t  msg_type;       /* MSG_ROOM_STATUS                       */
    uint8_t  room_id;        /* 1 or 2                                */
    uint8_t  occupied;       /* 1 if the room is occupied             */
    uint8_t  light_on;       /* 1 if the light is on                  */
    uint16_t motion_events;  /* cumulative motion count (diagnostics) */
    uint32_t uptime_sec;
} room_status_msg_t;

/* ---------------- Room controller API (room_main.c) ---------------- */
/* Starts one room controller task (PIR -> occupancy -> light).
 * status_pin < 0 means "no status LED". */
void room_start(int room_id, int pir_pin, int light_pin, int status_pin);

#ifdef STANDALONE
/* Implemented in entrance_main.c: hands a room status message to the
 * entrance through a FreeRTOS queue (replaces ESP-NOW on a single board). */
bool room_status_post(const room_status_msg_t *m);
#endif

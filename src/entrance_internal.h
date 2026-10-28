/* Internal interface for the three CoCIS application modules.
 * All shared state is defined exactly once in entrance_main.c.
 * Lock order: state_lock before sd_lock when both are required. */
#pragma once
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "driver/spi_master.h"
#include "driver/sdspi_host.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "esp_log.h"
#include "nvs.h"
#include "common.h"
#include "rc522.h"
#include "ds3231.h"
#include "lcd1602_i2c.h"


#define LOCK(m) xSemaphoreTake((m), portMAX_DELAY)
#define UNLOCK(m) xSemaphoreGive((m))
#define UID_FMT "%02X%02X%02X%02X"
#define UID_ARGS(u) (u)[0], (u)[1], (u)[2], (u)[3]
#define TABLE_MAX 32
#define LOGIN_MAX_FAILS 3
#define LOGIN_LOCK_MS 60000
#define DOOR_OPEN_MS 3000
#define ACCESS_COOLDOWN_MS 1500
#define BUILD_NAME "standalone (both rooms run as tasks on this ESP32)"

typedef struct {
    uint8_t  uid[4];
    char     name[24];
    uint32_t entry_min;          /* minutes since midnight at entry */
} person_t;

typedef struct {
    bool     online, occupied, light_on;
    uint16_t motion_events;
    /* energy accounting, in ms (see room_account) */
    uint32_t acct_tick, monitored_ms, light_ms, occ_ms;
} room_state_t;

extern SemaphoreHandle_t state_lock, sd_lock, lcd_lock, door_lock;
extern person_t inside_list[TABLE_MAX], tag_table[TABLE_MAX];
extern int inside_count, tag_count;
extern room_state_t room_states[3];
extern volatile int alarm_room;
extern char alarm_time[16];
extern int alarm_count;
extern TaskHandle_t alarm_task_handle;
extern bool override_on;
extern TickType_t override_until;
extern int login_fails;
extern TickType_t login_locked_until;
extern bool authed;
extern uint16_t open_min, close_min;
extern char admin_pass[32];
extern SemaphoreHandle_t rfid_lock;
extern rc522_t reader[2];
extern sdmmc_card_t *sdcard;
extern QueueHandle_t room_q;

/* Functions shared between application modules. */
const char *parse_uid(const char *s, uint8_t uid[4]);
void time_string(char *buf, size_t n);
int now_minutes(void);
bool within_hours(int minutes);
bool require_auth(void);
void sd_log(const char *fmt, ...);
void load_config(void);
void save_config(void);
int find_uid(const person_t *list, int n, const uint8_t uid[4]);
void person_set(person_t *p, const uint8_t uid[4], const char *name, uint32_t entry_min);
void list_remove(person_t *list, int *n, int i);
void tags_write_file(void);
void tags_load(void);
void occupancy_save_locked(void);
void occupancy_load(void);
bool lookup_tag(const uint8_t uid[4], char *name, size_t n);
bool is_inside(const uint8_t uid[4]);
bool occupancy_enter(const uint8_t uid[4], const char *name, uint32_t entry_min);
bool occupancy_leave(const uint8_t uid[4]);
int override_minutes_left(void);
void servo_angle(ledc_channel_t ch, int angle);
void servos_init(void);
void beep(int ms);
void blink(int gpio, int times, int ms);
void grant_access(const uint8_t uid[4], const char *name, bool entering, const char *note);
void deny_access(const uint8_t uid[4], const char *name, const char *why, const char *msg);
void process_tag(const uint8_t uid[4], bool in_lane);
void rfid_task(void *arg);
void alarm_task(void *arg);
void alert_intruder(int room);
void room_account(room_state_t *s, uint32_t now_tick);
void room_status_apply(const room_status_msg_t *m);
bool room_status_post(const room_status_msg_t *m);
void room_rx_task(void *arg);
void display_task(void *arg);
void cmd_help(char *args);
void cmd_status(char *args);
void cmd_inside(char *args);
void cmd_rooms(char *args);
void cmd_log(char *args);
void cmd_time(char *args);
void cmd_settime(char *args);
void cmd_setdate(char *args);
void cmd_schedule(char *args);
void set_schedule(const char *args, uint16_t *field);
void cmd_setopen(char *args);
void cmd_setclose(char *args);
void cmd_login(char *args);
void cmd_logout(char *args);
void cmd_passwd(char *args);
void fmt_hms(char *buf, size_t n, uint32_t ms);
void cmd_energy(char *args);
void cmd_ack(char *args);
void cmd_override(char *args);
void cmd_tap(char *args);
void cmd_tags(char *args);
void cmd_addtag(char *args);
void cmd_deltag(char *args);
void cmd_scan(char *args);
void run_command(char *line);
void menu_task(void *arg);
void sd_init(void);
void entrance_app_main(void);

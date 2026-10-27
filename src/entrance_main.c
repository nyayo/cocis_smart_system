/*
 * entrance_main.c - CoCIS Smart System MASTER terminal (main entrance)
 *
 *  - RFID access control at the IN and OUT lanes (2x RC522)
 *  - Access schedule (RTC) and admin password, stored in NVS
 *  - Occupancy counting + "who is inside" table
 *  - Room terminals push occupancy / lighting status to this terminal:
 *      STANDALONE build  : through a FreeRTOS queue (rooms are tasks)
 *      DISTRIBUTED build : through ESP-NOW (rooms are separate ESP32 boards)
 *  - Intruder alarm, lighting energy report
 *  - SD card logging (SINGLE WRITER => no file races)
 *  - LCD dashboard + serial management menu
 *
 * Mutexes (lock order is always state_lock -> sd_lock):
 *   state_lock  occupancy list, tag table, room states, alarm, override
 *   sd_lock     SD card file access
 *   lcd_lock    LCD
 *   door_lock   one access decision / door movement / alarm beep at a time
 */
#if defined(TERMINAL_ENTRANCE)

#include <stdio.h>
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
#ifndef STANDALONE
#include "esp_wifi.h"
#include "esp_now.h"
#endif
#include "common.h"
#include "rc522.h"
#include "ds3231.h"
#include "lcd1602_i2c.h"

static const char *TAG = "entrance";

#define LOCK(m)    xSemaphoreTake((m), portMAX_DELAY)
#define UNLOCK(m)  xSemaphoreGive((m))

/* printf helpers for a 4-byte tag UID, e.g.  printf(UID_FMT, UID_ARGS(uid)) */
#define UID_FMT      "%02X%02X%02X%02X"
#define UID_ARGS(u)  (u)[0], (u)[1], (u)[2], (u)[3]

#define TABLE_MAX         32     /* max people inside / max registered tags */
#define LOGIN_MAX_FAILS    3
#define LOGIN_LOCK_MS  60000
#define DOOR_OPEN_MS    3000
#define ACCESS_COOLDOWN_MS 1500

#ifdef STANDALONE
#define BUILD_NAME "standalone (both rooms run as tasks on this ESP32)"
#else
#define BUILD_NAME "distributed (rooms report over ESP-NOW)"
#endif

/* ============================== state =============================== */
static SemaphoreHandle_t state_lock, sd_lock, lcd_lock, door_lock;

/* One record type for both tables: "inside" uses entry_min, "tags" does not. */
typedef struct {
    uint8_t  uid[4];
    char     name[24];
    uint32_t entry_min;          /* minutes since midnight at entry */
} person_t;

static person_t inside_list[TABLE_MAX];   /* people inside (count == occupancy) */
static person_t tag_table[TABLE_MAX];     /* registered tags, mirrored on the SD card */
static int inside_count, tag_count;

/* Seed list, used only when the SD card has no tags file yet. Wokwi MFRC522
 * preset cards: Blue 01:02:03:04, Green 11:22:33:44, Red AA:BB:CC:DD.
 * Yellow (55:66:77:88) and the Key Fob (C0:FF:EE:99) are deliberately NOT
 * registered, so they demonstrate "unknown tag". */
static const struct { uint8_t uid[4]; const char *name; } DEFAULT_TAGS[] = {
    {{0x01, 0x02, 0x03, 0x04}, "Ivan Nyayo"},   /* Blue Card  */
    {{0x11, 0x22, 0x33, 0x44}, "Jane Staff"},   /* Green Card */
    {{0xAA, 0xBB, 0xCC, 0xDD}, "John Admin"},   /* Red Card   */
};

typedef struct {
    bool     online, occupied, light_on;
    uint16_t motion_events;
    /* energy accounting, in ms (see room_account) */
    uint32_t acct_tick, monitored_ms, light_ms, occ_ms;
} room_state_t;
static room_state_t room_states[3];       /* index 1..2 = rooms 1..2 */

static volatile int alarm_room;           /* 0 = none, else the room that raised it */
static char     alarm_time[16];
static int      alarm_count;
static TaskHandle_t alarm_task_handle;

static bool       override_on;            /* admin exit override */
static TickType_t override_until;

static int        login_fails;            /* menu task only */
static TickType_t login_locked_until;
static bool       authed;

static uint16_t open_min = 6 * 60 + 30, close_min = 21 * 60;   /* schedule, kept in NVS */
static char     admin_pass[32] = "admin123";

static rc522_t      reader[2];            /* [0] = IN lane, [1] = OUT lane */
static sdmmc_card_t *sdcard;

/* ============================== helpers ============================= */
static void time_string(char *buf, size_t n)
{
    struct tm t;
    ds3231_get_time(&t);
    snprintf(buf, n, "%04d-%02d-%02d %02d:%02d:%02d", t.tm_year + 1900, t.tm_mon + 1,
             t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec);
}

static int now_minutes(void)
{
    struct tm t;
    ds3231_get_time(&t);
    return t.tm_hour * 60 + t.tm_min;
}

static bool within_hours(int minutes) { return minutes >= open_min && minutes <= close_min; }

static bool require_auth(void)
{
    if (!authed) printf("DENIED: login first (login <password>)\n");
    return authed;
}

/* Parses 8 hex digits at the start of s into uid. Returns the position just
 * after them, or NULL if s does not start with a UID. */
static const char *parse_uid(const char *s, uint8_t uid[4])
{
    unsigned u[4];
    int n = 0;
    if (sscanf(s, "%2x%2x%2x%2x%n", &u[0], &u[1], &u[2], &u[3], &n) != 4) return NULL;
    for (int i = 0; i < 4; i++) uid[i] = (uint8_t)u[i];
    return s + n;
}

/* ------------------------------ SD log ------------------------------ */
static void sd_log(const char *fmt, ...)
{
    if (!sdcard) return;

    char line[192], ts[32];
    time_string(ts, sizeof(ts));
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);

    LOCK(sd_lock);
    FILE *f = fopen(LOG_FILE_PATH, "a");
    if (f) {
        fprintf(f, "%s,%s\n", ts, line);
        fclose(f);
    } else {
        ESP_LOGW(TAG, "cannot open %s for writing", LOG_FILE_PATH);
    }
    UNLOCK(sd_lock);
}

/* --------------------------- config (NVS) ---------------------------- */
static void load_config(void)
{
    nvs_handle_t h;
    if (nvs_open("cocis", NVS_READONLY, &h) != ESP_OK) return;
    size_t len = sizeof(admin_pass);
    nvs_get_u16(h, "open_min", &open_min);
    nvs_get_u16(h, "close_min", &close_min);
    nvs_get_str(h, "admin_pass", admin_pass, &len);
    nvs_close(h);
}

static void save_config(void)
{
    nvs_handle_t h;
    if (nvs_open("cocis", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u16(h, "open_min", open_min);
    nvs_set_u16(h, "close_min", close_min);
    nvs_set_str(h, "admin_pass", admin_pass);
    nvs_commit(h);
    nvs_close(h);
}

/* ------------------- people tables (CALLER HOLDS state_lock) -------- */
static int find_uid(const person_t *list, int n, const uint8_t uid[4])
{
    for (int i = 0; i < n; i++)
        if (memcmp(list[i].uid, uid, 4) == 0) return i;
    return -1;
}

static void person_set(person_t *p, const uint8_t uid[4], const char *name, uint32_t entry_min)
{
    memcpy(p->uid, uid, 4);
    snprintf(p->name, sizeof(p->name), "%s", name);
    p->entry_min = entry_min;
}

static void list_remove(person_t *list, int *n, int i)
{
    memmove(&list[i], &list[i + 1], (size_t)(*n - i - 1) * sizeof(person_t));
    (*n)--;
}

/* ------------------- tag registry on the SD card -------------------- */
/* Writes the whole registry to the SD card. CALLER HOLDS state_lock. */
static void tags_write_file(void)
{
    if (!sdcard) { printf("(SD card not mounted - tag list not saved)\n"); return; }

    LOCK(sd_lock);
    FILE *f = fopen(TAGS_FILE_PATH, "w");
    if (f) {
        for (int i = 0; i < tag_count; i++)
            fprintf(f, UID_FMT ",%s\n", UID_ARGS(tag_table[i].uid), tag_table[i].name);
        fclose(f);
    } else {
        ESP_LOGW(TAG, "cannot open %s for writing", TAGS_FILE_PATH);
    }
    UNLOCK(sd_lock);
}

/* Boot only (no other task exists yet): reads the registry from the SD card
 * and seeds it with DEFAULT_TAGS the first time (no file yet). */
static void tags_load(void)
{
    bool have_file = false;

    if (sdcard) {
        LOCK(sd_lock);
        FILE *f = fopen(TAGS_FILE_PATH, "r");
        if (f) {
            have_file = true;
            char line[64], name[24];
            uint8_t uid[4];
            while (tag_count < TABLE_MAX && fgets(line, sizeof(line), f)) {
                const char *p = parse_uid(line, uid);
                if (p && *p == ',' && sscanf(p + 1, "%23[^\r\n]", name) == 1)
                    person_set(&tag_table[tag_count++], uid, name, 0);
            }
            fclose(f);
        }
        UNLOCK(sd_lock);
    }

    if (!have_file) {
        for (size_t i = 0; i < sizeof(DEFAULT_TAGS) / sizeof(DEFAULT_TAGS[0]); i++)
            person_set(&tag_table[tag_count++], DEFAULT_TAGS[i].uid, DEFAULT_TAGS[i].name, 0);
        LOCK(state_lock);
        tags_write_file();
        UNLOCK(state_lock);
    }
    ESP_LOGI(TAG, "%d registered tag(s)", tag_count);
}

/* ------------------------- occupancy (mutex) ------------------------ */
static bool lookup_tag(const uint8_t uid[4], char *name, size_t n)
{
    LOCK(state_lock);
    int i = find_uid(tag_table, tag_count, uid);
    if (i >= 0) snprintf(name, n, "%s", tag_table[i].name);
    UNLOCK(state_lock);
    return i >= 0;
}

static bool is_inside(const uint8_t uid[4])
{
    LOCK(state_lock);
    int i = find_uid(inside_list, inside_count, uid);
    UNLOCK(state_lock);
    return i >= 0;
}

/* Returns false (and changes nothing) if the inside table is full. */
static bool occupancy_enter(const uint8_t uid[4], const char *name, uint32_t entry_min)
{
    LOCK(state_lock);
    bool ok = inside_count < TABLE_MAX;
    if (ok) person_set(&inside_list[inside_count++], uid, name, entry_min);
    UNLOCK(state_lock);
    return ok;
}

/* Returns false if the tag is not inside - the count never goes below 0. */
static bool occupancy_leave(const uint8_t uid[4])
{
    LOCK(state_lock);
    int i = find_uid(inside_list, inside_count, uid);
    if (i >= 0) list_remove(inside_list, &inside_count, i);
    UNLOCK(state_lock);
    return i >= 0;
}

/* Minutes left (rounded up) of an active admin exit override, or 0.
 * An expired override switches itself off here. */
static int override_minutes_left(void)
{
    int left = 0;
    LOCK(state_lock);
    if (override_on) {
        int32_t ticks = (int32_t)(override_until - xTaskGetTickCount());
        if (ticks > 0) left = (int)((((uint32_t)ticks * portTICK_PERIOD_MS) + 59999) / 60000);
        else           override_on = false;
    }
    UNLOCK(state_lock);
    return left;
}

/* -------------------- servos, buzzer, indicator LEDs ---------------- */
static void servo_angle(ledc_channel_t ch, int angle)
{
    /* 50 Hz, 14-bit: 0.5 ms..2.5 ms => duty 410..2048 */
    ledc_set_duty(LEDC_LOW_SPEED_MODE, ch, 410 + angle * (2048 - 410) / 180);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, ch);
}

static void servos_init(void)
{
    ledc_timer_config_t t = {
        .speed_mode      = LEDC_LOW_SPEED_MODE,
        .timer_num       = LEDC_TIMER_0,
        .duty_resolution = LEDC_TIMER_14_BIT,
        .freq_hz         = 50,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    ledc_timer_config(&t);

    const int pins[2] = {PIN_SERVO_IN, PIN_SERVO_OUT};   /* channel 0 = IN, 1 = OUT */
    for (int i = 0; i < 2; i++) {
        ledc_channel_config_t c = {
            .speed_mode = LEDC_LOW_SPEED_MODE,
            .channel    = (ledc_channel_t)i,
            .timer_sel  = LEDC_TIMER_0,
            .gpio_num   = pins[i],
        };
        ledc_channel_config(&c);
        servo_angle((ledc_channel_t)i, 0);
    }
}

static void beep(int ms)
{
    gpio_set_level(PIN_BUZZER, 1);
    vTaskDelay(pdMS_TO_TICKS(ms));
    gpio_set_level(PIN_BUZZER, 0);
}

static void blink(int gpio, int times, int ms)
{
    for (int i = 0; i < times; i++) {
        gpio_set_level(gpio, 1);
        vTaskDelay(pdMS_TO_TICKS(ms));
        gpio_set_level(gpio, 0);
        vTaskDelay(pdMS_TO_TICKS(ms));
    }
}

static void grant_access(const uint8_t uid[4], const char *name, bool entering, const char *note)
{
    sd_log("%s,%s," UID_FMT ",%s", entering ? "ENTRY" : "EXIT", name, UID_ARGS(uid), note);
    printf("[ACCESS] GRANTED: %s (%s)\n", name, entering ? "ENTRY" : "EXIT");
    blink(PIN_LED_GREEN, 2, 100);
    beep(80);
    ledc_channel_t ch = entering ? LEDC_CHANNEL_0 : LEDC_CHANNEL_1;
    servo_angle(ch, 90);                         /* open  */
    vTaskDelay(pdMS_TO_TICKS(DOOR_OPEN_MS));
    servo_angle(ch, 0);                          /* close */
}

/* why = text for the SD log, msg = text for the serial console */
static void deny_access(const uint8_t uid[4], const char *name, const char *why, const char *msg)
{
    sd_log("DENY,%s," UID_FMT ",%s", name, UID_ARGS(uid), why);
    printf("[ACCESS] DENIED: %s\n", msg);
    blink(PIN_LED_RED, 3, 100);
    beep(300);
}

/* --------------------------- access decision ------------------------ */
/* Called by the RFID task (real card) and by the serial "tap" command
 * (simulated card). door_lock lets only one decision / door movement run. */
static void process_tag(const uint8_t uid[4], bool in_lane)
{
    LOCK(door_lock);
    printf("[SCAN] %s lane, tag " UID_FMT "\n", in_lane ? "IN" : "OUT", UID_ARGS(uid));

    char name[24];
    bool known = lookup_tag(uid, name, sizeof(name));
    int  now   = now_minutes();                        /* read the clock ONCE */
    bool within = within_hours(now);
    /* An admin can allow EXITS (never entries) outside the schedule so that
     * nobody is locked in after closing time. */
    bool exit_override = !in_lane && !within && override_minutes_left() > 0;

    if (!known) {
        deny_access(uid, "UNKNOWN_UID", "tag not registered", "unknown tag");
    } else if (!within && !exit_override) {
        deny_access(uid, name, "outside schedule", "outside access hours");
    } else if (in_lane) {
        if (is_inside(uid))
            deny_access(uid, name, "already inside", "tag already inside (possible tailgating)");
        else if (!occupancy_enter(uid, name, (uint32_t)now))
            deny_access(uid, name, "occupancy table full", "occupancy table full");
        else
            grant_access(uid, name, true, "allowed");
    } else {
        if (!occupancy_leave(uid))
            deny_access(uid, name, "not inside (no matching entry)", "no matching entry record");
        else
            grant_access(uid, name, false, exit_override ? "allowed (admin exit override)" : "allowed");
    }
    UNLOCK(door_lock);
}

static void rfid_task(void *arg)
{
    while (1) {
        for (int lane = 0; lane < 2; lane++) {
            uint8_t uid[4];
            if (rc522_read_uid(&reader[lane], uid)) {
                process_tag(uid, lane == 0);
                vTaskDelay(pdMS_TO_TICKS(ACCESS_COOLDOWN_MS));
            }
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }
}

/* ------------------------ intruder alarm ---------------------------- */
/* Beeps / flashes the red LED for ~10 s. The alarm itself stays LATCHED (shown
 * on the LCD) until an admin runs "ack". door_lock is taken for each beep so
 * the alarm never fights the door logic over the same LED / buzzer pins. */
static void alarm_task(void *arg)
{
    while (1) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        for (int i = 0; i < 25 && alarm_room != 0; i++) {
            LOCK(door_lock);
            gpio_set_level(PIN_LED_RED, 1);
            gpio_set_level(PIN_BUZZER, 1);
            vTaskDelay(pdMS_TO_TICKS(200));
            gpio_set_level(PIN_LED_RED, 0);
            gpio_set_level(PIN_BUZZER, 0);
            UNLOCK(door_lock);
            vTaskDelay(pdMS_TO_TICKS(200));
        }
    }
}

/* Motion in a room while nobody is registered inside means somebody got in
 * without scanning a tag. */
static void alert_intruder(int room)
{
    char ts[32];
    time_string(ts, sizeof(ts));                       /* "YYYY-MM-DD HH:MM:SS" */

    LOCK(state_lock);
    alarm_room = room;
    alarm_count++;
    snprintf(alarm_time, sizeof(alarm_time), "%.8s", ts + 11);   /* HH:MM:SS */
    UNLOCK(state_lock);

    printf("[ALERT] INTRUDER? Motion in Room %d but nobody is registered inside the building!\n", room);
    sd_log("ALERT,INTRUDER,Room %d,motion with nobody registered inside", room);
    if (alarm_task_handle) xTaskNotifyGive(alarm_task_handle);
}

/* ------------------------ energy accounting ------------------------- */
/* Credits the time since the last step to the room's CURRENT state.
 * CALLER HOLDS state_lock. */
static void room_account(room_state_t *s, uint32_t now_tick)
{
    if (!s->online) return;
    uint32_t dt = (now_tick - s->acct_tick) * portTICK_PERIOD_MS;
    s->acct_tick = now_tick;
    s->monitored_ms += dt;
    if (s->light_on) s->light_ms += dt;
    if (s->occupied) s->occ_ms   += dt;
}

/* ------------------------ room status intake ------------------------ */
/* Shared by both builds: validates a room status message and updates the room
 * table. STANDALONE calls it from room_rx_task (queue), the DISTRIBUTED build
 * from the ESP-NOW receive callback. */
static void room_status_apply(const room_status_msg_t *m)
{
    if (m->magic != COCIS_MAGIC || m->msg_type != MSG_ROOM_STATUS) return;
    if (m->room_id < 1 || m->room_id > 2) return;

    LOCK(state_lock);
    room_state_t *rs = &room_states[m->room_id];
    uint32_t now = xTaskGetTickCount();

    bool intruder = m->occupied && !rs->occupied && inside_count == 0;
    bool changed  = rs->occupied != (bool)m->occupied || rs->light_on != (bool)m->light_on;

    if (rs->online) room_account(rs, now);      /* credit the OLD state first */
    else            rs->acct_tick = now;        /* first message: start counting */

    rs->online        = true;
    rs->occupied      = m->occupied;
    rs->light_on      = m->light_on;
    rs->motion_events = m->motion_events;
    UNLOCK(state_lock);

    if (changed) {
        printf("[ROOM%d] %s, light %s (motion events: %u)\n", m->room_id,
               m->occupied ? "OCCUPIED" : "EMPTY", m->light_on ? "ON" : "OFF", m->motion_events);
        sd_log("ROOM,%d,%s,%s", m->room_id, m->occupied ? "OCCUPIED" : "EMPTY",
               m->light_on ? "LIGHT_ON" : "LIGHT_OFF");
    }
    if (intruder) alert_intruder(m->room_id);
}

#ifdef STANDALONE
/* ---- single-board build: rooms are tasks, status travels in a queue ---- */
static QueueHandle_t room_q;

bool room_status_post(const room_status_msg_t *m)
{
    return room_q != NULL && xQueueSend(room_q, m, 0) == pdTRUE;
}

static void room_rx_task(void *arg)
{
    room_status_msg_t m;
    while (1)
        if (xQueueReceive(room_q, &m, portMAX_DELAY) == pdTRUE) room_status_apply(&m);
}
#else
/* ---- distributed build: rooms are separate boards, status via ESP-NOW --- */
static void on_espnow_recv(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
    if (len == sizeof(room_status_msg_t)) room_status_apply((const room_status_msg_t *)data);
}

static void wifi_espnow_init(void)
{
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    uint8_t mac[] = ENTRANCE_MAC_ADDR;
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_mac(WIFI_IF_STA, mac));   /* before start */
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_recv_cb(on_espnow_recv));
    ESP_LOGI(TAG, "ESP-NOW ready, MAC %02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}
#endif /* STANDALONE */

/* ------------------------------ LCD task ---------------------------- */
/* 16x2 LCD: the dashboard rotates through 2 pages (3 while an alarm is latched). */
static const char *room_label(const room_state_t *s)
{
    return !s->online ? "--" : !s->occupied ? "OFF" : s->light_on ? "ON" : "OCC";
}

static void display_task(void *arg)
{
    for (unsigned tick = 0;; tick++) {
        struct tm t;
        room_state_t rs[3];
        char alarm_ts[16];

        ds3231_get_time(&t);
        LOCK(state_lock);
        int occ = inside_count, alarm = alarm_room;
        snprintf(alarm_ts, sizeof(alarm_ts), "%s", alarm_time);
        memcpy(rs, room_states, sizeof(rs));
        UNLOCK(state_lock);

        bool within = within_hours(t.tm_hour * 60 + t.tm_min);
        bool ovr = override_minutes_left() > 0;
        unsigned page = (tick / 6) % (alarm ? 3 : 2);

        LOCK(lcd_lock);
        if (page == 0) {
            lcd_printf(0, 0, "%02d:%02d:%02d IN:%-2d", t.tm_hour, t.tm_min, t.tm_sec, occ);
            lcd_printf(1, 0, "R1:%-3s R2:%-3s", room_label(&rs[1]), room_label(&rs[2]));
        } else if (page == 1) {
            lcd_printf(0, 0, "Hrs %02d:%02d-%02d:%02d", open_min / 60, open_min % 60,
                       close_min / 60, close_min % 60);
            lcd_printf(1, 0, "Door: %s", within ? "OPEN" : (ovr ? "EXIT ONLY" : "CLOSED"));
        } else {
            lcd_printf(0, 0, "!! INTRUDER !!");
            lcd_printf(1, 0, "R%d @%s", alarm, alarm_ts);
        }
        UNLOCK(lcd_lock);

        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

/* ============================ SERIAL MENU ============================ */
static void cmd_help(char *args)
{
    printf("\n==== CoCIS Smart System - Main Menu ====\n"
           "  status              system overview\n"
           "  inside              who is inside the building\n"
           "  rooms               room occupancy / lighting status\n"
           "  log                 print access log from SD card\n"
           "  time                show RTC time\n"
           "  settime HH MM SS    set RTC time\n"
           "  setdate DD MM YY    set RTC date\n"
           "  schedule            show access schedule\n"
           "  setopen HH:MM       set opening time   (auth)\n"
           "  setclose HH:MM      set closing time   (auth)\n"
           "  login <password>    authenticate admin (3 wrong tries = 60 s lockout)\n"
           "  logout              end the admin session\n"
           "  energy              lighting energy saved by the occupancy control\n"
           "  energy reset        clear the energy counters        (auth)\n"
           "  override <min|off>  allow EXITS outside hours, 1-120 min (auth)\n"
           "  ack                 acknowledge an intruder alert    (auth)\n"
           "  passwd <new>        change admin password (auth)\n"
           "  tap in|out <UID>    simulate a card (test), e.g. tap in 01020304 (auth)\n"
           "  tags                list registered tags            (auth)\n"
           "  addtag <UID> <name> register a tag, e.g. addtag 55667788 Visitor (auth)\n"
           "  deltag <UID>        remove a tag                    (auth)\n"
           "  scan                print RFID UIDs for 15 s        (auth)\n"
           "  help                show this menu\n\n");
}

static void cmd_status(char *args)
{
    struct tm t;
    room_state_t rs[3];
    char alarm_ts[16];

    ds3231_get_time(&t);
    int now = t.tm_hour * 60 + t.tm_min;
    LOCK(state_lock);
    int occ = inside_count, alarm = alarm_room, alarms = alarm_count;
    snprintf(alarm_ts, sizeof(alarm_ts), "%s", alarm_time);
    memcpy(rs, room_states, sizeof(rs));
    UNLOCK(state_lock);
    int ovr = override_minutes_left();

    printf("---- STATUS ----\n");
    printf("Build        : " BUILD_NAME "\n");
    printf("Clock source : %s\n", ds3231_available() ? "RTC chip (I2C 0x68)" : "software clock (no RTC found)");
    printf("RTC time     : %04d-%02d-%02d %02d:%02d:%02d\n",
           t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec);
    printf("Schedule     : %02d:%02d - %02d:%02d (now %02d:%02d) => %s\n",
           open_min / 60, open_min % 60, close_min / 60, close_min % 60,
           t.tm_hour, t.tm_min, within_hours(now) ? "OPEN" : "CLOSED");
    printf("Occupancy    : %d inside\n", occ);
    for (int r = 1; r <= 2; r++)
        printf("Room %d       : %s, light %s\n", r,
               rs[r].occupied ? "OCCUPIED" : "EMPTY", rs[r].light_on ? "ON" : "OFF");
    printf("SD card      : %s\n", sdcard ? "mounted" : "NOT mounted");
    printf("Admin auth   : %s\n", authed ? "yes" : "no");
    if (ovr > 0) printf("Exit override: ON (about %d min left)\n", ovr);
    else         printf("Exit override: off\n");
    if (alarm)
        printf("Intruder alarm: LATCHED - Room %d at %s (use 'ack'), %d alert(s) since boot\n",
               alarm, alarm_ts, alarms);
    else
        printf("Intruder alarm: none (%d alert(s) since boot)\n", alarms);
}

static void cmd_inside(char *args)
{
    LOCK(state_lock);
    printf("---- INSIDE (%d) ----\n", inside_count);
    for (int i = 0; i < inside_count; i++)
        printf("  %-20s since %02u:%02u  uid:" UID_FMT "\n", inside_list[i].name,
               (unsigned)(inside_list[i].entry_min / 60), (unsigned)(inside_list[i].entry_min % 60),
               UID_ARGS(inside_list[i].uid));
    UNLOCK(state_lock);
}

static void cmd_rooms(char *args)
{
    for (int r = 1; r <= 2; r++) {
        LOCK(state_lock);
        room_state_t s = room_states[r];
        UNLOCK(state_lock);
        printf("Room %d: %s | %s | motion events: %u\n", r, s.online ? "online" : "OFFLINE",
               s.occupied ? (s.light_on ? "OCCUPIED, light ON" : "OCCUPIED, light OFF")
                          : (s.light_on ? "EMPTY, light ON (!)" : "EMPTY, light OFF"),
               s.motion_events);
    }
}

static void cmd_log(char *args)
{
    if (!sdcard) { printf("SD card not mounted.\n"); return; }

    LOCK(sd_lock);
    FILE *f = fopen(LOG_FILE_PATH, "r");
    if (f) {
        char line[160];
        while (fgets(line, sizeof(line), f)) printf("%s", line);
        fclose(f);
    } else {
        printf("Log file not found (nothing has been logged yet).\n");
    }
    UNLOCK(sd_lock);
}

static void cmd_time(char *args)
{
    char ts[32];
    time_string(ts, sizeof(ts));
    printf("%s\n", ts);
}

static void cmd_settime(char *args)
{
    int h, m, s;
    if (sscanf(args, "%d %d %d", &h, &m, &s) != 3) { printf("Usage: settime HH MM SS\n"); return; }
    struct tm t;
    ds3231_get_time(&t);
    t.tm_hour = h; t.tm_min = m; t.tm_sec = s;
    ds3231_set_time(&t);
    printf("RTC time set.\n");
}

static void cmd_setdate(char *args)
{
    int d, mo, y;
    if (sscanf(args, "%d %d %d", &d, &mo, &y) != 3) { printf("Usage: setdate DD MM YY\n"); return; }
    struct tm t;
    ds3231_get_time(&t);
    t.tm_mday = d; t.tm_mon = mo - 1; t.tm_year = y + 100;
    ds3231_set_time(&t);
    printf("RTC date set.\n");
}

static void cmd_schedule(char *args)
{
    printf("Access schedule: %02d:%02d - %02d:%02d\n",
           open_min / 60, open_min % 60, close_min / 60, close_min % 60);
}

static void set_schedule(const char *args, uint16_t *field)
{
    int h, m;
    const char *what = (field == &open_min) ? "open" : "close";
    if (sscanf(args, "%d:%d", &h, &m) != 2) { printf("Usage: set%s HH:MM\n", what); return; }
    *field = (uint16_t)(h * 60 + m);
    save_config();
    printf("%s time set to %02d:%02d\n", (field == &open_min) ? "Opening" : "Closing", h, m);
}
static void cmd_setopen(char *args)  { set_schedule(args, &open_min); }
static void cmd_setclose(char *args) { set_schedule(args, &close_min); }

static void cmd_login(char *args)
{
    TickType_t now = xTaskGetTickCount();

    if (login_fails >= LOGIN_MAX_FAILS) {
        int32_t left = (int32_t)(login_locked_until - now);
        if (left > 0) {
            printf("Login locked after too many wrong passwords. Try again in %d s.\n",
                   (int)(((uint32_t)left * portTICK_PERIOD_MS) / 1000) + 1);
            return;
        }
        login_fails = 0;                                 /* lockout is over */
    }

    if (strcmp(args, admin_pass) == 0) {
        authed = true;
        login_fails = 0;
        sd_log("ADMIN,LOGIN_OK,-,-");
        printf("Authenticated.\n");
        return;
    }

    authed = false;
    login_fails++;
    sd_log("ADMIN,LOGIN_FAILED,attempt %d,-", login_fails);
    if (login_fails >= LOGIN_MAX_FAILS) {
        login_locked_until = now + pdMS_TO_TICKS(LOGIN_LOCK_MS);
        sd_log("ADMIN,LOGIN_LOCKOUT,%d s,-", LOGIN_LOCK_MS / 1000);
        printf("Wrong password. Login locked for %d s.\n", LOGIN_LOCK_MS / 1000);
    } else {
        printf("Wrong password (%d of %d attempts).\n", login_fails, LOGIN_MAX_FAILS);
    }
}

static void cmd_logout(char *args)
{
    authed = false;
    printf("Logged out.\n");
}

static void cmd_passwd(char *args)
{
    size_t n = strlen(args);
    if (n == 0 || n >= sizeof(admin_pass)) {
        printf("Password must be 1-%d characters.\n", (int)sizeof(admin_pass) - 1);
        return;
    }
    memcpy(admin_pass, args, n + 1);
    save_config();
    printf("Password changed.\n");
}

/* --------------------------- energy report -------------------------- */
static void fmt_hms(char *buf, size_t n, uint32_t ms)
{
    uint32_t s = ms / 1000;
    snprintf(buf, n, "%02u : %02u : %02u", (unsigned)(s / 3600), (unsigned)((s / 60) % 60), (unsigned)(s % 60));
}

/* Energy saved by switching lights off in empty rooms, compared with the
 * lights simply staying on for the whole time the room was monitored. */
static void cmd_energy(char *args)
{
    room_state_t snap[3];

    if (!strcmp(args, "reset")) {
        if (!require_auth()) return;
        LOCK(state_lock);
        for (int r = 1; r <= 2; r++) {
            room_state_t *s = &room_states[r];
            s->monitored_ms = s->light_ms = s->occ_ms = 0;
            s->acct_tick = xTaskGetTickCount();
        }
        UNLOCK(state_lock);
        sd_log("ADMIN,ENERGY_RESET,-,-");
        printf("Energy counters cleared.\n");
        return;
    }

    LOCK(state_lock);
    for (int r = 1; r <= 2; r++) {
        room_account(&room_states[r], xTaskGetTickCount());   /* credit up to this moment */
        snap[r] = room_states[r];
    }
    UNLOCK(state_lock);

    printf("---- ENERGY REPORT (assumed %d W per room light) ----\n", LIGHT_WATTS);
    uint32_t tot_mon = 0, tot_saved = 0;
    for (int r = 1; r <= 2; r++) {
        const room_state_t *s = &snap[r];
        if (!s->online) { printf("Room %d: no data yet (room offline)\n", r); continue; }

        uint32_t saved = (s->monitored_ms > s->light_ms) ? s->monitored_ms - s->light_ms : 0;
        unsigned pct = s->monitored_ms ? (unsigned)(((uint64_t)saved * 100) / s->monitored_ms) : 0;
        unsigned long long mwh = (unsigned long long)saved * LIGHT_WATTS / 3600;
        char a[16], b[16], c[16], d[16];
        fmt_hms(a, sizeof(a), s->monitored_ms);
        fmt_hms(b, sizeof(b), s->light_ms);
        fmt_hms(c, sizeof(c), s->occ_ms);
        fmt_hms(d, sizeof(d), saved);
        printf("Room %d: monitored %s | light on %s | occupied %s\n", r, a, b, c);
        printf("        light off for %s = %u%% saved, about %llu.%03llu Wh\n",
               d, pct, mwh / 1000, mwh % 1000);
        tot_mon += s->monitored_ms;
        tot_saved += saved;
    }
    unsigned long long tot_mwh = (unsigned long long)tot_saved * LIGHT_WATTS / 3600;
    printf("-- TOTAL : about %llu.%03llu Wh saved over %u%% of the monitored time\n",
           tot_mwh / 1000, tot_mwh % 1000,
           tot_mon ? (unsigned)(((uint64_t)tot_saved * 100) / tot_mon) : 0);
    printf("(baseline = light left on the whole time; counters restart at reboot)\n");
}

/* ---------------------- alarm / override / tap ---------------------- */
static void cmd_ack(char *args)
{
    LOCK(state_lock);
    int room = alarm_room;
    alarm_room = 0;
    UNLOCK(state_lock);

    if (room) {
        sd_log("ADMIN,ALARM_ACK,Room %d,-", room);
        printf("Intruder alarm for Room %d acknowledged.\n", room);
    } else {
        printf("No alarm to acknowledge.\n");
    }
}

static void cmd_override(char *args)
{
    int minutes = 0;
    bool off = !strcmp(args, "off");

    if (!off && !(sscanf(args, "%d", &minutes) == 1 && minutes >= 1 && minutes <= 120)) {
        printf("Usage: override <1-120 minutes> | override off\n"
               "Allows EXITS outside the schedule, but not ENTRIES.\n");
        return;
    }

    LOCK(state_lock);
    override_on = !off;
    if (!off) override_until = xTaskGetTickCount() + pdMS_TO_TICKS((uint32_t)minutes * 60000);
    UNLOCK(state_lock);

    if (off) {
        sd_log("ADMIN,EXIT_OVERRIDE_OFF,-,-");
        printf("Exit override switched off.\n");
    } else {
        sd_log("ADMIN,EXIT_OVERRIDE_ON,%d min,-", minutes);
        printf("Exits allowed outside hours for %d min (entries stay blocked).\n", minutes);
    }
}

static void cmd_tap(char *args)
{
    char lane[4];
    uint8_t uid[4];
    int n = 0;

    if (sscanf(args, "%3s%n", lane, &n) == 1 && (!strcmp(lane, "in") || !strcmp(lane, "out")) &&
        parse_uid(args + n, uid))
        process_tag(uid, lane[0] == 'i');
    else
        printf("Usage: tap in|out <8 hex digits>   e.g. tap in 01020304\n");
}

/* --------------------------- tag management ------------------------- */
static void cmd_tags(char *args)
{
    LOCK(state_lock);
    printf("---- REGISTERED TAGS (%d/%d) ----\n", tag_count, TABLE_MAX);
    for (int i = 0; i < tag_count; i++)
        printf("  " UID_FMT "  %s\n", UID_ARGS(tag_table[i].uid), tag_table[i].name);
    UNLOCK(state_lock);
}

static void cmd_addtag(char *args)
{
    uint8_t uid[4];
    char name[24], existing[24];
    const char *p = parse_uid(args, uid);

    if (!p || sscanf(p, " %23[^\r\n]", name) != 1 || strchr(name, ',')) {
        printf("Usage: addtag <8 hex digits> <name>   e.g. addtag 55667788 Visitor (no commas)\n");
        return;
    }
    if (lookup_tag(uid, existing, sizeof(existing))) {
        printf("Tag already registered to %s\n", existing);
        return;
    }

    LOCK(state_lock);
    bool full = tag_count >= TABLE_MAX;
    if (!full) {
        person_set(&tag_table[tag_count++], uid, name, 0);
        tags_write_file();
    }
    UNLOCK(state_lock);

    if (full) { printf("Tag table full (%d tags).\n", TABLE_MAX); return; }
    sd_log("ADMIN,TAG_ADDED," UID_FMT ",%s", UID_ARGS(uid), name);
    printf("Registered " UID_FMT " as %s\n", UID_ARGS(uid), name);
}

static void cmd_deltag(char *args)
{
    uint8_t uid[4];

    if (!parse_uid(args, uid)) { printf("Usage: deltag <8 hex digits>\n"); return; }
    if (is_inside(uid)) {
        printf("That tag is currently inside the building - let it exit first.\n");
        return;
    }

    LOCK(state_lock);
    int i = find_uid(tag_table, tag_count, uid);
    if (i >= 0) {
        list_remove(tag_table, &tag_count, i);
        tags_write_file();
    }
    UNLOCK(state_lock);

    if (i < 0) { printf("No such tag.\n"); return; }
    sd_log("ADMIN,TAG_REMOVED," UID_FMT ",-", UID_ARGS(uid));
    printf("Removed tag " UID_FMT "\n", UID_ARGS(uid));
}

static void cmd_scan(char *args)
{
    printf("Scanning for 15 seconds - present tags to either reader...\n");
    TickType_t end = xTaskGetTickCount() + pdMS_TO_TICKS(15000);
    while (xTaskGetTickCount() < end) {
        uint8_t uid[4];
        if (rc522_read_uid(&reader[0], uid) || rc522_read_uid(&reader[1], uid))
            printf("  UID: " UID_FMT "\n", UID_ARGS(uid));
        vTaskDelay(pdMS_TO_TICKS(300));
    }
}

/* ---------------------------- command table ------------------------- */
typedef struct {
    const char *name;
    void      (*fn)(char *args);
    bool        needs_auth;
} command_t;

static const command_t COMMANDS[] = {
    {"help",     cmd_help,     false}, {"status",  cmd_status,   false},
    {"inside",   cmd_inside,   false}, {"rooms",   cmd_rooms,    false},
    {"log",      cmd_log,      false}, {"time",    cmd_time,     false},
    {"settime",  cmd_settime,  false}, {"setdate", cmd_setdate,  false},
    {"schedule", cmd_schedule, false}, {"login",   cmd_login,    false},
    {"logout",   cmd_logout,   false}, {"energy",  cmd_energy,   false},  /* "energy reset" checks auth itself */
    {"setopen",  cmd_setopen,  true},  {"setclose", cmd_setclose, true},
    {"override", cmd_override, true},  {"ack",     cmd_ack,      true},
    {"passwd",   cmd_passwd,   true},  {"tap",     cmd_tap,      true},
    {"tags",     cmd_tags,     true},  {"addtag",  cmd_addtag,   true},
    {"deltag",   cmd_deltag,   true},  {"scan",    cmd_scan,     true},
};

/* Splits "name args" at the first space and runs the matching command. */
static void run_command(char *line)
{
    char *args = strchr(line, ' ');
    if (args) *args++ = '\0';
    else      args = line + strlen(line);          /* empty argument string */

    for (size_t i = 0; i < sizeof(COMMANDS) / sizeof(COMMANDS[0]); i++) {
        if (strcmp(line, COMMANDS[i].name) != 0) continue;
        if (COMMANDS[i].needs_auth && !require_auth()) return;
        COMMANDS[i].fn(args);
        return;
    }
    printf("Unknown command. Type 'help'.\n");
}

static void menu_task(void *arg)
{
    char line[96] = {0};
    size_t len = 0;

    cmd_help(NULL);
    while (1) {
        int c = getchar();
        if (c == EOF) { vTaskDelay(pdMS_TO_TICKS(20)); continue; }

        if (c == 0x08 || c == 0x7F) {                      /* backspace */
            if (len > 0) { line[--len] = '\0'; printf("\b \b"); }
        } else if (c != '\n' && c != '\r') {
            putchar(c);                                    /* local echo */
            if (len < sizeof(line) - 1) { line[len++] = (char)c; line[len] = '\0'; }
        } else {
            putchar('\n');
            if (len > 0) run_command(line);
            len = 0;
            line[0] = '\0';
        }
    }
}

/* ------------------------------- init ------------------------------- */
static void sd_init(void)
{
    spi_bus_config_t bus = {
        .mosi_io_num     = PIN_SD_MOSI,
        .miso_io_num     = PIN_SD_MISO,
        .sclk_io_num     = PIN_SD_SCK,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = 4000,
    };
    esp_err_t ret = spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "SD SPI bus init failed: %s", esp_err_to_name(ret));
        return;
    }

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SPI2_HOST;
    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.gpio_cs  = PIN_SD_CS;
    slot.host_id  = SPI2_HOST;
    esp_vfs_fat_sdmmc_mount_config_t mount = {
        .format_if_mount_failed = true,
        .max_files              = 5,
        .allocation_unit_size   = 16 * 1024,
    };

    ret = esp_vfs_fat_sdspi_mount("/sdcard", &host, &slot, &mount, &sdcard);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SD mount failed: %s", esp_err_to_name(ret));
        sdcard = NULL;
        return;
    }
    sdmmc_card_print_info(stdout, sdcard);
}

void entrance_app_main(void)
{
    state_lock = xSemaphoreCreateMutex();
    sd_lock    = xSemaphoreCreateMutex();
    lcd_lock   = xSemaphoreCreateMutex();
    door_lock  = xSemaphoreCreateMutex();

    load_config();
    gpio_config_t out = {
        .pin_bit_mask = (1ULL << PIN_BUZZER) | (1ULL << PIN_LED_GREEN) | (1ULL << PIN_LED_RED),
        .mode         = GPIO_MODE_OUTPUT,
    };
    gpio_config(&out);

    ds3231_init();                       /* also installs the I2C driver the LCD uses */
    lcd_init(0x27);
    lcd_printf(0, 0, "CoCIS SMART SYS");
    lcd_printf(1, 0, "Booting...");

    /* both RC522 readers share one SPI bus (VSPI / SPI3) */
    spi_bus_config_t bus = {
        .mosi_io_num     = PIN_RC522_MOSI,
        .miso_io_num     = PIN_RC522_MISO,
        .sclk_io_num     = PIN_RC522_SCK,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = 64,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &bus, SPI_DMA_CH_AUTO));
    rc522_init(&reader[0], SPI3_HOST, PIN_RC522_IN_CS,  PIN_RC522_IN_RST);
    rc522_init(&reader[1], SPI3_HOST, PIN_RC522_OUT_CS, PIN_RC522_OUT_RST);

    servos_init();
    sd_init();
    tags_load();
    xTaskCreate(alarm_task, "alarm", 2048, NULL, 4, &alarm_task_handle);

#ifdef STANDALONE
    /* The rooms run as tasks on this ESP32 and report through a queue. */
    room_q = xQueueCreate(8, sizeof(room_status_msg_t));
    xTaskCreate(room_rx_task, "room_rx", 4096, NULL, 4, NULL);
    room_start(1, PIN_ROOM1_PIR, PIN_ROOM1_LIGHT, -1);
    room_start(2, PIN_ROOM2_PIR, PIN_ROOM2_LIGHT, -1);
    ESP_LOGI(TAG, "STANDALONE build: rooms 1 and 2 run as tasks (queue instead of ESP-NOW)");
#else
    wifi_espnow_init();
#endif

    xTaskCreate(rfid_task,    "rfid",    4096, NULL, 5, NULL);
    xTaskCreate(display_task, "display", 4096, NULL, 3, NULL);
    xTaskCreate(menu_task,    "menu",    6144, NULL, 4, NULL);
    ESP_LOGI(TAG, "Entrance terminal ready. Default admin password: admin123");
}

#endif /* TERMINAL_ENTRANCE */

/* RFID, registered tags, occupancy, door control and access decisions. */
#include "entrance_internal.h"
static const char *TAG = "entrance";

/* Seed list, used only when the SD card has no tags file yet. Wokwi MFRC522
 * preset cards: Blue 01:02:03:04, Green 11:22:33:44, Red AA:BB:CC:DD.
 * Yellow (55:66:77:88) and the Key Fob (C0:FF:EE:99) are deliberately NOT
 * registered, so they demonstrate "unknown tag". */
static const struct { uint8_t uid[4]; const char *name; } DEFAULT_TAGS[] = {
    {{0x01, 0x02, 0x03, 0x04}, "Ivan Nyayo"},   /* Blue Card  */
    {{0x11, 0x22, 0x33, 0x44}, "Jane Staff"},   /* Green Card */
    {{0xAA, 0xBB, 0xCC, 0xDD}, "John Admin"},   /* Red Card   */
};

/* ------------------- people tables (CALLER HOLDS state_lock) -------- */
int find_uid(const person_t *list, int n, const uint8_t uid[4])
{
    for (int i = 0; i < n; i++)
        if (memcmp(list[i].uid, uid, 4) == 0) return i;
    return -1;
}

void person_set(person_t *p, const uint8_t uid[4], const char *name, uint32_t entry_min)
{
    memcpy(p->uid, uid, 4);
    snprintf(p->name, sizeof(p->name), "%s", name);
    p->entry_min = entry_min;
}

void list_remove(person_t *list, int *n, int i)
{
    memmove(&list[i], &list[i + 1], (size_t)(*n - i - 1) * sizeof(person_t));
    (*n)--;
}

/* ------------------- tag registry on the SD card -------------------- */
/* Writes the whole registry to the SD card. CALLER HOLDS state_lock. */
void tags_write_file(void)
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
void tags_load(void)
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

/* Occupancy snapshot is kept in NVS so a reset does not erase the roster.
 * Save only when a person enters/exits, not on every room heartbeat. */
void occupancy_save_locked(void)
{
    nvs_handle_t h;
    if (nvs_open("cocis_occ", NVS_READWRITE, &h) != ESP_OK) return;
    esp_err_t err = nvs_set_blob(h, "roster", inside_list,
                                  (size_t)inside_count * sizeof(person_t));
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) ESP_LOGE(TAG, "Failed to persist occupancy: %s", esp_err_to_name(err));
}

void occupancy_load(void)
{
    nvs_handle_t h;
    if (nvs_open("cocis_occ", NVS_READONLY, &h) != ESP_OK) return;
    size_t len = sizeof(inside_list);
    esp_err_t err = nvs_get_blob(h, "roster", inside_list, &len);
    nvs_close(h);
    if (err == ESP_OK && len % sizeof(person_t) == 0 && len <= sizeof(inside_list)) {
        inside_count = (int)(len / sizeof(person_t));
        ESP_LOGI(TAG, "Restored %d people inside from NVS", inside_count);
    } else if (err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "Invalid occupancy snapshot; starting with empty roster");
    }
}

/* ------------------------- occupancy (mutex) ------------------------ */
bool lookup_tag(const uint8_t uid[4], char *name, size_t n)
{
    LOCK(state_lock);
    int i = find_uid(tag_table, tag_count, uid);
    if (i >= 0) snprintf(name, n, "%s", tag_table[i].name);
    UNLOCK(state_lock);
    return i >= 0;
}

bool is_inside(const uint8_t uid[4])
{
    LOCK(state_lock);
    int i = find_uid(inside_list, inside_count, uid);
    UNLOCK(state_lock);
    return i >= 0;
}

/* Returns false (and changes nothing) if the inside table is full. */
bool occupancy_enter(const uint8_t uid[4], const char *name, uint32_t entry_min)
{
    LOCK(state_lock);
    bool ok = inside_count < TABLE_MAX;
    if (ok) { person_set(&inside_list[inside_count++], uid, name, entry_min); occupancy_save_locked(); }
    UNLOCK(state_lock);
    return ok;
}

/* Returns false if the tag is not inside - the count never goes below 0. */
bool occupancy_leave(const uint8_t uid[4])
{
    LOCK(state_lock);
    int i = find_uid(inside_list, inside_count, uid);
    if (i >= 0) { list_remove(inside_list, &inside_count, i); occupancy_save_locked(); }
    UNLOCK(state_lock);
    return i >= 0;
}

/* Minutes left (rounded up) of an active admin exit override, or 0.
 * An expired override switches itself off here. */
int override_minutes_left(void)
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
void servo_angle(ledc_channel_t ch, int angle)
{
    /* 50 Hz, 14-bit: 0.5 ms..2.5 ms => duty 410..2048 */
    ledc_set_duty(LEDC_LOW_SPEED_MODE, ch, 410 + angle * (2048 - 410) / 180);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, ch);
}

void servos_init(void)
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

void beep(int ms)
{
    gpio_set_level(PIN_BUZZER, 1);
    vTaskDelay(pdMS_TO_TICKS(ms));
    gpio_set_level(PIN_BUZZER, 0);
}

void blink(int gpio, int times, int ms)
{
    for (int i = 0; i < times; i++) {
        gpio_set_level(gpio, 1);
        vTaskDelay(pdMS_TO_TICKS(ms));
        gpio_set_level(gpio, 0);
        vTaskDelay(pdMS_TO_TICKS(ms));
    }
}

void grant_access(const uint8_t uid[4], const char *name, bool entering, const char *note)
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
void deny_access(const uint8_t uid[4], const char *name, const char *why, const char *msg)
{
    sd_log("DENY,%s," UID_FMT ",%s", name, UID_ARGS(uid), why);
    printf("[ACCESS] DENIED: %s\n", msg);
    blink(PIN_LED_RED, 3, 100);
    beep(300);
}

/* --------------------------- access decision ------------------------ */
/* Called by the RFID task (real card) and by the serial "tap" command
 * (simulated card). door_lock lets only one decision / door movement run. */
void process_tag(const uint8_t uid[4], bool in_lane)
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

void rfid_task(void *arg)
{
    while (1) {
        for (int lane = 0; lane < 2; lane++) {
            uint8_t uid[4];
            LOCK(rfid_lock);
            bool found = rc522_read_uid(&reader[lane], uid);
            UNLOCK(rfid_lock);
            if (found) {
                process_tag(uid, lane == 0);
                vTaskDelay(pdMS_TO_TICKS(ACCESS_COOLDOWN_MS));
            }
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }
}


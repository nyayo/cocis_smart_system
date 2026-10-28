/* Persistent configuration, RTC helpers, SD logging and serial commands. */
#include "entrance_internal.h"
static const char *TAG = "entrance";

/* ============================== helpers ============================= */
void time_string(char *buf, size_t n)
{
    struct tm t;
    ds3231_get_time(&t);
    snprintf(buf, n, "%04d-%02d-%02d %02d:%02d:%02d", t.tm_year + 1900, t.tm_mon + 1,
             t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec);
}

int now_minutes(void)
{
    struct tm t;
    ds3231_get_time(&t);
    return t.tm_hour * 60 + t.tm_min;
}

bool within_hours(int minutes) { return minutes >= open_min && minutes <= close_min; }

bool require_auth(void)
{
    if (!authed) printf("DENIED: login first (login <password>)\n");
    return authed;
}

/* Parses 8 hex digits at the start of s into uid. Returns the position just
 * after them, or NULL if s does not start with a UID. */
const char *parse_uid(const char *s, uint8_t uid[4])
{
    unsigned u[4];
    int n = 0;
    if (sscanf(s, "%2x%2x%2x%2x%n", &u[0], &u[1], &u[2], &u[3], &n) != 4) return NULL;
    for (int i = 0; i < 4; i++) uid[i] = (uint8_t)u[i];
    return s + n;
}

/* ------------------------------ SD log ------------------------------ */
void sd_log(const char *fmt, ...)
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
void load_config(void)
{
    nvs_handle_t h;
    if (nvs_open("cocis", NVS_READONLY, &h) != ESP_OK) return;
    size_t len = sizeof(admin_pass);
    nvs_get_u16(h, "open_min", &open_min);
    nvs_get_u16(h, "close_min", &close_min);
    nvs_get_str(h, "admin_pass", admin_pass, &len);
    nvs_close(h);
    if (open_min >= 1440 || close_min >= 1440 || open_min >= close_min) {
        ESP_LOGW(TAG, "Invalid saved schedule; restoring 06:30-21:00");
        open_min = 390; close_min = 1260;
    }
}

void save_config(void)
{
    nvs_handle_t h;
    if (nvs_open("cocis", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u16(h, "open_min", open_min);
    nvs_set_u16(h, "close_min", close_min);
    nvs_set_str(h, "admin_pass", admin_pass);
    nvs_commit(h);
    nvs_close(h);
}


/* ============================ SERIAL MENU ============================ */
void cmd_help(char *args)
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

void cmd_status(char *args)
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

void cmd_inside(char *args)
{
    LOCK(state_lock);
    printf("---- INSIDE (%d) ----\n", inside_count);
    for (int i = 0; i < inside_count; i++)
        printf("  %-20s since %02u:%02u  uid:" UID_FMT "\n", inside_list[i].name,
               (unsigned)(inside_list[i].entry_min / 60), (unsigned)(inside_list[i].entry_min % 60),
               UID_ARGS(inside_list[i].uid));
    UNLOCK(state_lock);
}

void cmd_rooms(char *args)
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

void cmd_log(char *args)
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

void cmd_time(char *args)
{
    char ts[32];
    time_string(ts, sizeof(ts));
    printf("%s\n", ts);
}

void cmd_settime(char *args)
{
    int h, m, s;
    if (sscanf(args, "%d %d %d", &h, &m, &s) != 3 || h < 0 || h > 23 || m < 0 || m > 59 || s < 0 || s > 59) { printf("Usage: settime HH MM SS (valid 24-hour time)\n"); return; }
    struct tm t;
    ds3231_get_time(&t);
    t.tm_hour = h; t.tm_min = m; t.tm_sec = s;
    if (ds3231_set_time(&t) == ESP_OK) { sd_log("ADMIN,SET_TIME,-,-"); printf("RTC time set.\n"); }
    else printf("RTC time update failed.\n");
}

void cmd_setdate(char *args)
{
    int d, mo, y;
    if (sscanf(args, "%d %d %d", &d, &mo, &y) != 3 || y < 0 || y > 99 || mo < 1 || mo > 12 || d < 1 || d > 31) { printf("Usage: setdate DD MM YY\n"); return; }
    struct tm valid = { .tm_year = y + 100, .tm_mon = mo - 1, .tm_mday = d, .tm_hour = 12 };
    mktime(&valid);
    if (valid.tm_mday != d || valid.tm_mon != mo - 1 || valid.tm_year != y + 100) { printf("Invalid calendar date.\n"); return; }
    struct tm t;
    ds3231_get_time(&t);
    t.tm_mday = d; t.tm_mon = mo - 1; t.tm_year = y + 100;
    if (ds3231_set_time(&t) == ESP_OK) { sd_log("ADMIN,SET_DATE,-,-"); printf("RTC date set.\n"); }
    else printf("RTC date update failed.\n");
}

void cmd_schedule(char *args)
{
    printf("Access schedule: %02d:%02d - %02d:%02d\n",
           open_min / 60, open_min % 60, close_min / 60, close_min % 60);
}

void set_schedule(const char *args, uint16_t *field)
{
    int h, m;
    const char *what = (field == &open_min) ? "open" : "close";
    char extra;
    if (sscanf(args, "%d:%d %c", &h, &m, &extra) != 2 || h < 0 || h > 23 || m < 0 || m > 59) {
        printf("Usage: set%s HH:MM (00:00 to 23:59)\n", what); return;
    }
    int candidate = h * 60 + m;
    if ((field == &open_min && candidate >= close_min) ||
        (field == &close_min && candidate <= open_min)) {
        printf("Opening must be earlier than closing (same-day schedule).\n"); return;
    }
    *field = (uint16_t)candidate;
    save_config();
    printf("%s time set to %02d:%02d\n", (field == &open_min) ? "Opening" : "Closing", h, m);
}
void cmd_setopen(char *args)  { set_schedule(args, &open_min); }
void cmd_setclose(char *args) { set_schedule(args, &close_min); }

void cmd_login(char *args)
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

void cmd_logout(char *args)
{
    authed = false;
    printf("Logged out.\n");
}

void cmd_passwd(char *args)
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
void fmt_hms(char *buf, size_t n, uint32_t ms)
{
    uint32_t s = ms / 1000;
    snprintf(buf, n, "%02u : %02u : %02u", (unsigned)(s / 3600), (unsigned)((s / 60) % 60), (unsigned)(s % 60));
}

/* Energy saved by switching lights off in empty rooms, compared with the
 * lights simply staying on for the whole time the room was monitored. */
void cmd_energy(char *args)
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
void cmd_ack(char *args)
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

void cmd_override(char *args)
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

void cmd_tap(char *args)
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
void cmd_tags(char *args)
{
    LOCK(state_lock);
    printf("---- REGISTERED TAGS (%d/%d) ----\n", tag_count, TABLE_MAX);
    for (int i = 0; i < tag_count; i++)
        printf("  " UID_FMT "  %s\n", UID_ARGS(tag_table[i].uid), tag_table[i].name);
    UNLOCK(state_lock);
}

void cmd_addtag(char *args)
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

void cmd_deltag(char *args)
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

void cmd_scan(char *args)
{
    printf("Scanning for 15 seconds - present tags to either reader...\n");
    TickType_t end = xTaskGetTickCount() + pdMS_TO_TICKS(15000);
    while (xTaskGetTickCount() < end) {
        uint8_t uid[4];
        LOCK(rfid_lock);
        bool found = rc522_read_uid(&reader[0], uid) || rc522_read_uid(&reader[1], uid);
        UNLOCK(rfid_lock);
        if (found) printf("  UID: " UID_FMT "\n", UID_ARGS(uid));
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
    {"settime",  cmd_settime,  true}, {"setdate", cmd_setdate,  true},
    {"schedule", cmd_schedule, false}, {"login",   cmd_login,    false},
    {"logout",   cmd_logout,   false}, {"energy",  cmd_energy,   false},  /* "energy reset" checks auth itself */
    {"setopen",  cmd_setopen,  true},  {"setclose", cmd_setclose, true},
    {"override", cmd_override, true},  {"ack",     cmd_ack,      true},
    {"passwd",   cmd_passwd,   true},  {"tap",     cmd_tap,      true},
    {"tags",     cmd_tags,     true},  {"addtag",  cmd_addtag,   true},
    {"deltag",   cmd_deltag,   true},  {"scan",    cmd_scan,     true},
};

/* Splits "name args" at the first space and runs the matching command. */
void run_command(char *line)
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

void menu_task(void *arg)
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
void sd_init(void)
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
        .format_if_mount_failed = false,
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


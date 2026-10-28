# CoCIS Smart Control and Monitoring System — Modular ESP-IDF Code Explanation
**Study guide and line-numbered source reference**  
**Prepared:** 28 October 2026  
**Code basis:** Working five-file modular patch supplied in `CoCIS_Modular_Changed_Files_Only.zip`.

> **Scope:** This guide explains the exact five affected files in the modular patch. `main.c`, `room_main.c`, `common.h`, and the RC522/RTC/LCD drivers were not included in that patch, so their complete line-by-line contents are not reproduced or assumed here. All included code is copied from the actual files, with original line numbers. The project is reported working by the developer; this guide is not a substitute for hardware or simulator testing.

## 1. Project overview
The single ESP32 runs an entrance/exit controller plus two independent PIR-controlled lecture rooms using ESP-IDF and FreeRTOS. Two RC522 readers share an SPI bus, two servos simulate doors, a DS3231 supplies time, an LCD displays status, SD storage holds CSV logs and registered tags, and NVS retains settings and occupancy across resets. Room tasks report status through a FreeRTOS queue. Serial commands allow administrators to inspect and manage the system.

### Runtime flow
```text
ESP32 boot -> app_main() [unchanged main.c]
  -> NVS initialization
  -> entrance_app_main() [entrance_main.c]
     -> create mutexes and load persisted state
     -> initialize GPIO, I2C RTC/LCD, SPI RFID, PWM servos, SD
     -> create alarm task and room status queue
     -> start Room 1 and Room 2 tasks
     -> create room receiver, RFID polling, LCD and serial menu tasks
```

### Module responsibilities
| File | Responsibility |
|---|---|
| `entrance_main.c` | Owns global state, coordinates startup, processes room messages, alarm and LCD tasks |
| `access_control.c` | RFID tag registry, occupancy, servos, access decisions and RFID polling |
| `system_manager.c` | RTC helpers, configuration, SD logging, serial commands and SD initialization |
| `entrance_internal.h` | Shared types, constants, extern declarations and cross-module function prototypes |
| `CMakeLists.txt` | Registers source files and ESP-IDF dependencies |

## 2. Core concepts for the lecturer
**ESP-IDF:** Native Espressif development framework. `app_main()` is the application entry point; it is not an Arduino `setup()/loop()` sketch. **FreeRTOS task:** independently scheduled function with its own stack and priority. **Queue:** transfers copies of room status messages from producers to a consumer. **Mutex:** serializes access to shared state or devices. **NVS:** key/value flash storage for small persistent state. **SPI:** bus used for RC522 and separately for SD. **I²C:** bus used by DS3231 and LCD. **LEDC PWM:** hardware pulse generation for servo angle control.

### Important concurrency invariants
- `state_lock` protects the roster, tag table, room state, alarm metadata and some override state.
- `sd_lock` serializes SD file access; `lcd_lock` serializes LCD output; `door_lock` serializes door access/feedback; `rfid_lock` serializes physical RFID reads.
- When both state and SD locks are needed, acquire **state before SD**, as stated in the header. Reversing lock order can deadlock.
- `occupancy_save_locked()` assumes its caller already owns `state_lock`; calling it unsafely can cause a race.
- The room queue is capacity eight and sends with zero wait; if full, a status message may be dropped.
- `vTaskDelay(pdMS_TO_TICKS(ms))` yields a task; it is not a busy-wait.

## 3. Source code explained by module and function
### 3.1 `entrance_internal.h`
This header is included by all three application `.c` files. `#pragma once` avoids repeated inclusion. `extern` declares variables defined once in `entrance_main.c`; it does not allocate another copy. The `LOCK` and `UNLOCK` macros wrap FreeRTOS semaphore calls. `person_t` stores UID/name/entry time, and `room_state_t` stores room status and energy-accounting counters. `TABLE_MAX` caps registered and inside tables at 32. The prototypes expose the functions that modules call across file boundaries.
**Line-reading hints:** A `#define` is a compile-time macro; `typedef struct` declares a reusable data type; `extern` is a declaration; function declarations ending in `;` specify interfaces rather than implementations.

### 3.2 `entrance_main.c`
The following function map gives the behavior of **every named function in this module**. The complete source, numbered line by line, follows in Section 4.

| Function (first line) | Detailed purpose |
|---|---|
| `alarm_task()` (L39) | Waits for a task notification, then flashes the red LED and sounds the buzzer in short pulses. The door mutex prevents conflicting buzzer/indicator operations. |
| `alert_intruder()` (L58) | Records a latched intrusion alert when a room first reports motion while the building roster is empty; logs it and wakes the alarm task. |
| `room_account()` (L77) | Adds elapsed time to monitored, light-on, and occupied counters based on the previous room state. Caller must hold state_lock. |
| `room_status_apply()` (L89) | Rejects malformed room messages, snapshots old state, accounts elapsed energy time, updates the room state under a mutex, logs changes, and triggers alerts. |
| `room_status_post()` (L120) | Enqueues a room status message without waiting; may fail if the queue is full. |
| `room_rx_task()` (L126) | Blocks waiting for queue messages and passes each to room_status_apply. |
| `room_label()` (L137) | Maps room online/occupied/light flags to compact LCD text. |
| `display_task()` (L141) | Every 500 ms reads RTC and synchronized room/occupancy snapshots, rotates LCD pages, and shows a latched alarm when present. |
| `entrance_app_main()` (L177) | Creates five mutexes, loads NVS state, initializes GPIO/I2C/SPI/RC522/servos/SD, creates the room queue and all FreeRTOS tasks. |

**Detailed logic:** The module declares exactly one instance of each global shared object. `alarm_task()` blocks on `ulTaskNotifyTake`, avoiding CPU use until notified. `room_status_apply()` validates message type and room ID before locking, calls `room_account()` **before** changing the state so the previous interval is counted correctly, then logs outside the state lock. `display_task()` snapshots state under lock and releases it before updating the LCD. Startup creates mutexes before using any shared services, then creates the queue before room tasks can send.

### 3.3 `access_control.c`
The following function map gives the behavior of **every named function in this module**. The complete source, numbered line by line, follows in Section 4.

| Function (first line) | Detailed purpose |
|---|---|
| `find_uid()` (L16) | Linear search through a person/tag table, comparing four UID bytes; returns index or -1. |
| `person_set()` (L22) | Copies the UID and bounded name into a person record and saves the entry minute. |
| `list_remove()` (L29) | Compacts an array using memmove and decreases the count. |
| `tags_write_file()` (L38) | Writes the full registered-tag table to CSV on SD while holding the SD mutex. |
| `tags_load()` (L56) | Loads registered tags from SD; seeds three demo tags only when the file does not exist. |
| `occupancy_save_locked()` (L89) | Serializes the current roster as an NVS blob and commits it. The caller must already own state_lock. |
| `occupancy_load()` (L99) | Restores the occupancy roster from NVS and validates the blob length. |
| `lookup_tag()` (L116) | Looks up a registered UID under state_lock and copies its name if found. |
| `is_inside()` (L124) | Tests whether a UID already appears in the current occupancy roster. |
| `occupancy_enter()` (L134) | Adds a person when capacity allows and persists the roster; returns false when full. |
| `occupancy_leave()` (L144) | Removes a matching person and persists the roster; returns false when not inside. |
| `override_minutes_left()` (L155) | Computes remaining time for an admin exit override, rounding up to whole minutes, and expires it automatically. |
| `servo_angle()` (L169) | Maps an angle to LEDC PWM duty for a 50 Hz servo signal. |
| `servos_init()` (L175) | Configures one LEDC timer and two channels for entry and exit servos. |
| `beep()` (L199) | Sets buzzer HIGH, waits for the requested duration, then sets LOW. |
| `blink()` (L206) | Repeatedly switches a GPIO output on and off. |
| `grant_access()` (L216) | Writes an ENTRY/EXIT log, gives green feedback, opens the appropriate servo, waits three seconds, and closes it. |
| `deny_access()` (L230) | Logs denial and gives red LED and buzzer feedback. |
| `process_tag()` (L241) | Central access decision: serialize with door_lock; validate tag, schedule, override, occupancy; then grant or deny. |
| `rfid_task()` (L273) | Continuously polls both readers under rfid_lock and applies a cooldown after a detected card. |

**Detailed access decision:** `process_tag()` locks `door_lock` for the whole decision. It looks up the tag, reads the RTC once, checks normal hours or exit-only override, rejects unknown tags, rejects repeated entry, rejects exit without matching entry, and only then invokes the servo/feedback function. `occupancy_enter()` and `occupancy_leave()` mutate the roster under `state_lock` and save an NVS snapshot. `rfid_task()` locks the reader interface around each physical scan and delays after detection to reduce repeat scans.
**Demo cards:** The seed list contains `01020304`, `11223344`, and `AABBCCDD`. Other example tags are intentionally unregistered for denial tests. The default registry is seeded only when the SD tags file is missing.

### 3.4 `system_manager.c`
The following function map gives the behavior of **every named function in this module**. The complete source, numbered line by line, follows in Section 4.

| Function (first line) | Detailed purpose |
|---|---|
| `time_string()` (L6) | Reads RTC time and formats a timestamp for logs. |
| `now_minutes()` (L13) | Converts RTC hour and minute to minutes since midnight. |
| `within_hours()` (L20) | Returns whether the given minute lies between opening and closing times, inclusive. |
| `require_auth()` (L22) | Checks whether the serial administrator session is authenticated. |
| `parse_uid()` (L31) | Parses four hexadecimal UID bytes and returns the position after the UID. |
| `sd_log()` (L41) | Formats a timestamped CSV log line and appends it to the SD card while holding sd_lock. |
| `load_config()` (L64) | Loads opening/closing schedule and admin password from NVS, resetting an invalid schedule. |
| `save_config()` (L78) | Persists the schedule and admin password to NVS and commits. |
| `cmd_help()` (L92) | Prints available serial commands and their syntax. |
| `cmd_status()` (L119) | Displays RTC, schedule, occupancy, rooms, SD status, authentication, override and alarm summary. |
| `cmd_inside()` (L157) | Prints the names, entry times and UIDs of people currently inside. |
| `cmd_rooms()` (L168) | Prints room connectivity, occupancy, light state and motion event counts. |
| `cmd_log()` (L181) | Reads the stored SD access log. |
| `cmd_time()` (L197) | Prints current RTC date and time. |
| `cmd_settime()` (L204) | Validates HH MM SS and updates the RTC; review authentication enforcement in command dispatch. |
| `cmd_setdate()` (L215) | Validates date including calendar normalization and updates RTC; review authentication enforcement in command dispatch. |
| `cmd_schedule()` (L229) | Displays configured opening and closing times. |
| `set_schedule()` (L235) | Parses a new opening/closing time, validates it and saves the updated schedule. |
| `cmd_setopen()` (L253) | Delegates opening-time changes to set_schedule. |
| `cmd_setclose()` (L254) | Delegates closing-time changes to set_schedule. |
| `cmd_login()` (L255) | Authenticates the administrator and implements failure count and temporary lockout. |
| `cmd_logout()` (L289) | Ends the authenticated serial session. |
| `cmd_passwd()` (L295) | Changes and persists the administrator password after authorization. |
| `fmt_hms()` (L309) | Formats a millisecond duration as hours/minutes/seconds. |
| `cmd_energy()` (L317) | Reports lighting runtime and estimated savings based on occupancy-based switching; can reset counters. |
| `cmd_ack()` (L370) | Acknowledges and clears a latched intrusion alarm. |
| `cmd_override()` (L384) | Allows a bounded-duration exit-only schedule override for an authenticated administrator. |
| `cmd_tap()` (L409) | Simulates an RFID card tap from serial input for testing. |
| `cmd_tags()` (L424) | Lists registered RFID tags. |
| `cmd_addtag()` (L432) | Registers a UID and name, respecting capacity and persistence. |
| `cmd_deltag()` (L460) | Removes a registered tag and updates SD persistence. |
| `cmd_scan()` (L483) | Temporarily reads and prints RFID UIDs for administrative registration. |
| `run_command()` (L520) | Tokenizes the serial command and dispatches it to the corresponding handler. |
| `menu_task()` (L534) | Reads serial console input and calls run_command in a loop. |
| `sd_init()` (L560) | Initializes and mounts the SD card, reporting failures rather than silently formatting it. |

**Detailed command handling:** The serial menu is a command dispatcher: parse a command word, select a `cmd_*` handler, validate arguments, check authentication for privileged operations, and print results. `sd_log()` uses a variable-argument list (`va_start`/`vsnprintf`/`va_end`) to write timestamped CSV lines. `load_config()`/`save_config()` read and write NVS keys. `set_schedule()` checks valid opening/closing order. `cmd_energy()` uses accumulated light-on and occupied durations to estimate lighting savings. Review the `run_command()` dispatch conditions when explaining which commands are protected.

### 3.5 `CMakeLists.txt`
`idf_component_register()` tells the ESP-IDF build system which `.c` files to compile. `INCLUDE_DIRS "."` makes local headers available. `REQUIRES` declares dependent ESP-IDF components. Forgetting a new `.c` file here can lead to undefined-reference linker errors.

## 4. Complete line-numbered source listings
These listings are the **actual contents of the modified files**, not reconstructed pseudocode. Read line N using the module explanation above. Comments, braces, declarations and blank lines are preserved; line numbers refer to each original file.
### `entrance_internal.h` — 135 lines
```c
   1 | /* Internal interface for the three CoCIS application modules.
   2 |  * All shared state is defined exactly once in entrance_main.c.
   3 |  * Lock order: state_lock before sd_lock when both are required. */
   4 | #pragma once
   5 | #include <stdio.h>
   6 | #include <stdlib.h>
   7 | #include <string.h>
   8 | #include <stdarg.h>
   9 | #include <time.h>
  10 | #include "freertos/FreeRTOS.h"
  11 | #include "freertos/task.h"
  12 | #include "freertos/semphr.h"
  13 | #include "freertos/queue.h"
  14 | #include "driver/spi_master.h"
  15 | #include "driver/sdspi_host.h"
  16 | #include "driver/gpio.h"
  17 | #include "driver/ledc.h"
  18 | #include "esp_vfs_fat.h"
  19 | #include "sdmmc_cmd.h"
  20 | #include "esp_log.h"
  21 | #include "nvs.h"
  22 | #include "common.h"
  23 | #include "rc522.h"
  24 | #include "ds3231.h"
  25 | #include "lcd1602_i2c.h"
  26 | 
  27 | 
  28 | #define LOCK(m) xSemaphoreTake((m), portMAX_DELAY)
  29 | #define UNLOCK(m) xSemaphoreGive((m))
  30 | #define UID_FMT "%02X%02X%02X%02X"
  31 | #define UID_ARGS(u) (u)[0], (u)[1], (u)[2], (u)[3]
  32 | #define TABLE_MAX 32
  33 | #define LOGIN_MAX_FAILS 3
  34 | #define LOGIN_LOCK_MS 60000
  35 | #define DOOR_OPEN_MS 3000
  36 | #define ACCESS_COOLDOWN_MS 1500
  37 | #define BUILD_NAME "standalone (both rooms run as tasks on this ESP32)"
  38 | 
  39 | typedef struct {
  40 |     uint8_t  uid[4];
  41 |     char     name[24];
  42 |     uint32_t entry_min;          /* minutes since midnight at entry */
  43 | } person_t;
  44 | 
  45 | typedef struct {
  46 |     bool     online, occupied, light_on;
  47 |     uint16_t motion_events;
  48 |     /* energy accounting, in ms (see room_account) */
  49 |     uint32_t acct_tick, monitored_ms, light_ms, occ_ms;
  50 | } room_state_t;
  51 | 
  52 | extern SemaphoreHandle_t state_lock, sd_lock, lcd_lock, door_lock;
  53 | extern person_t inside_list[TABLE_MAX], tag_table[TABLE_MAX];
  54 | extern int inside_count, tag_count;
  55 | extern room_state_t room_states[3];
  56 | extern volatile int alarm_room;
  57 | extern char alarm_time[16];
  58 | extern int alarm_count;
  59 | extern TaskHandle_t alarm_task_handle;
  60 | extern bool override_on;
  61 | extern TickType_t override_until;
  62 | extern int login_fails;
  63 | extern TickType_t login_locked_until;
  64 | extern bool authed;
  65 | extern uint16_t open_min, close_min;
  66 | extern char admin_pass[32];
  67 | extern SemaphoreHandle_t rfid_lock;
  68 | extern rc522_t reader[2];
  69 | extern sdmmc_card_t *sdcard;
  70 | extern QueueHandle_t room_q;
  71 | 
  72 | /* Functions shared between application modules. */
  73 | const char *parse_uid(const char *s, uint8_t uid[4]);
  74 | void time_string(char *buf, size_t n);
  75 | int now_minutes(void);
  76 | bool within_hours(int minutes);
  77 | bool require_auth(void);
  78 | void sd_log(const char *fmt, ...);
  79 | void load_config(void);
  80 | void save_config(void);
  81 | int find_uid(const person_t *list, int n, const uint8_t uid[4]);
  82 | void person_set(person_t *p, const uint8_t uid[4], const char *name, uint32_t entry_min);
  83 | void list_remove(person_t *list, int *n, int i);
  84 | void tags_write_file(void);
  85 | void tags_load(void);
  86 | void occupancy_save_locked(void);
  87 | void occupancy_load(void);
  88 | bool lookup_tag(const uint8_t uid[4], char *name, size_t n);
  89 | bool is_inside(const uint8_t uid[4]);
  90 | bool occupancy_enter(const uint8_t uid[4], const char *name, uint32_t entry_min);
  91 | bool occupancy_leave(const uint8_t uid[4]);
  92 | int override_minutes_left(void);
  93 | void servo_angle(ledc_channel_t ch, int angle);
  94 | void servos_init(void);
  95 | void beep(int ms);
  96 | void blink(int gpio, int times, int ms);
  97 | void grant_access(const uint8_t uid[4], const char *name, bool entering, const char *note);
  98 | void deny_access(const uint8_t uid[4], const char *name, const char *why, const char *msg);
  99 | void process_tag(const uint8_t uid[4], bool in_lane);
 100 | void rfid_task(void *arg);
 101 | void alarm_task(void *arg);
 102 | void alert_intruder(int room);
 103 | void room_account(room_state_t *s, uint32_t now_tick);
 104 | void room_status_apply(const room_status_msg_t *m);
 105 | bool room_status_post(const room_status_msg_t *m);
 106 | void room_rx_task(void *arg);
 107 | void display_task(void *arg);
 108 | void cmd_help(char *args);
 109 | void cmd_status(char *args);
 110 | void cmd_inside(char *args);
 111 | void cmd_rooms(char *args);
 112 | void cmd_log(char *args);
 113 | void cmd_time(char *args);
 114 | void cmd_settime(char *args);
 115 | void cmd_setdate(char *args);
 116 | void cmd_schedule(char *args);
 117 | void set_schedule(const char *args, uint16_t *field);
 118 | void cmd_setopen(char *args);
 119 | void cmd_setclose(char *args);
 120 | void cmd_login(char *args);
 121 | void cmd_logout(char *args);
 122 | void cmd_passwd(char *args);
 123 | void fmt_hms(char *buf, size_t n, uint32_t ms);
 124 | void cmd_energy(char *args);
 125 | void cmd_ack(char *args);
 126 | void cmd_override(char *args);
 127 | void cmd_tap(char *args);
 128 | void cmd_tags(char *args);
 129 | void cmd_addtag(char *args);
 130 | void cmd_deltag(char *args);
 131 | void cmd_scan(char *args);
 132 | void run_command(char *line);
 133 | void menu_task(void *arg);
 134 | void sd_init(void);
 135 | void entrance_app_main(void);
```

### `entrance_main.c` — 237 lines
```c
   1 | #include "entrance_internal.h"
   2 | static const char *TAG = "entrance";
   3 | 
   4 | /* ============================== state =============================== */
   5 | SemaphoreHandle_t state_lock, sd_lock, lcd_lock, door_lock;
   6 | 
   7 | person_t inside_list[TABLE_MAX];   /* people inside (count == occupancy) */
   8 | person_t tag_table[TABLE_MAX];     /* registered tags, mirrored on the SD card */
   9 | int inside_count, tag_count;
  10 | 
  11 | 
  12 | room_state_t room_states[3];       /* index 1..2 = rooms 1..2 */
  13 | 
  14 | volatile int alarm_room;           /* 0 = none, else the room that raised it */
  15 | char     alarm_time[16];
  16 | int      alarm_count;
  17 | TaskHandle_t alarm_task_handle;
  18 | 
  19 | bool       override_on;            /* admin exit override */
  20 | TickType_t override_until;
  21 | 
  22 | int        login_fails;            /* menu task only */
  23 | TickType_t login_locked_until;
  24 | bool       authed;
  25 | 
  26 | uint16_t open_min = 6 * 60 + 30, close_min = 21 * 60;   /* schedule, kept in NVS */
  27 | char     admin_pass[32] = "admin123";
  28 | 
  29 | SemaphoreHandle_t rfid_lock;
  30 | rc522_t      reader[2];            /* [0] = IN lane, [1] = OUT lane */
  31 | sdmmc_card_t *sdcard;
  32 | 
  33 | QueueHandle_t room_q;
  34 | 
  35 | /* ------------------------ intruder alarm ---------------------------- */
  36 | /* Beeps / flashes the red LED for ~10 s. The alarm itself stays LATCHED (shown
  37 |  * on the LCD) until an admin runs "ack". door_lock is taken for each beep so
  38 |  * the alarm never fights the door logic over the same LED / buzzer pins. */
  39 | void alarm_task(void *arg)
  40 | {
  41 |     while (1) {
  42 |         ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
  43 |         for (int i = 0; i < 25 && alarm_room != 0; i++) {
  44 |             LOCK(door_lock);
  45 |             gpio_set_level(PIN_LED_RED, 1);
  46 |             gpio_set_level(PIN_BUZZER, 1);
  47 |             vTaskDelay(pdMS_TO_TICKS(200));
  48 |             gpio_set_level(PIN_LED_RED, 0);
  49 |             gpio_set_level(PIN_BUZZER, 0);
  50 |             UNLOCK(door_lock);
  51 |             vTaskDelay(pdMS_TO_TICKS(200));
  52 |         }
  53 |     }
  54 | }
  55 | 
  56 | /* Motion in a room while nobody is registered inside means somebody got in
  57 |  * without scanning a tag. */
  58 | void alert_intruder(int room)
  59 | {
  60 |     char ts[32];
  61 |     time_string(ts, sizeof(ts));                       /* "YYYY-MM-DD HH:MM:SS" */
  62 | 
  63 |     LOCK(state_lock);
  64 |     alarm_room = room;
  65 |     alarm_count++;
  66 |     snprintf(alarm_time, sizeof(alarm_time), "%.8s", ts + 11);   /* HH:MM:SS */
  67 |     UNLOCK(state_lock);
  68 | 
  69 |     printf("[ALERT] INTRUDER? Motion in Room %d but nobody is registered inside the building!\n", room);
  70 |     sd_log("ALERT,INTRUDER,Room %d,motion with nobody registered inside", room);
  71 |     if (alarm_task_handle) xTaskNotifyGive(alarm_task_handle);
  72 | }
  73 | 
  74 | /* ------------------------ energy accounting ------------------------- */
  75 | /* Credits the time since the last step to the room's CURRENT state.
  76 |  * CALLER HOLDS state_lock. */
  77 | void room_account(room_state_t *s, uint32_t now_tick)
  78 | {
  79 |     if (!s->online) return;
  80 |     uint32_t dt = (now_tick - s->acct_tick) * portTICK_PERIOD_MS;
  81 |     s->acct_tick = now_tick;
  82 |     s->monitored_ms += dt;
  83 |     if (s->light_on) s->light_ms += dt;
  84 |     if (s->occupied) s->occ_ms   += dt;
  85 | }
  86 | 
  87 | /* ------------------------ room status intake ------------------------ */
  88 | /* Validate room task messages before updating shared state. */
  89 | void room_status_apply(const room_status_msg_t *m)
  90 | {
  91 |     if (m->magic != COCIS_MAGIC || m->msg_type != MSG_ROOM_STATUS) return;
  92 |     if (m->room_id < 1 || m->room_id > 2) return;
  93 | 
  94 |     LOCK(state_lock);
  95 |     room_state_t *rs = &room_states[m->room_id];
  96 |     uint32_t now = xTaskGetTickCount();
  97 | 
  98 |     bool intruder = m->occupied && !rs->occupied && inside_count == 0;
  99 |     bool changed  = rs->occupied != (bool)m->occupied || rs->light_on != (bool)m->light_on;
 100 | 
 101 |     if (rs->online) room_account(rs, now);      /* credit the OLD state first */
 102 |     else            rs->acct_tick = now;        /* first message: start counting */
 103 | 
 104 |     rs->online        = true;
 105 |     rs->occupied      = m->occupied;
 106 |     rs->light_on      = m->light_on;
 107 |     rs->motion_events = m->motion_events;
 108 |     UNLOCK(state_lock);
 109 | 
 110 |     if (changed) {
 111 |         printf("[ROOM%d] %s, light %s (motion events: %u)\n", m->room_id,
 112 |                m->occupied ? "OCCUPIED" : "EMPTY", m->light_on ? "ON" : "OFF", m->motion_events);
 113 |         sd_log("ROOM,%d,%s,%s", m->room_id, m->occupied ? "OCCUPIED" : "EMPTY",
 114 |                m->light_on ? "LIGHT_ON" : "LIGHT_OFF");
 115 |     }
 116 |     if (intruder) alert_intruder(m->room_id);
 117 | }
 118 | 
 119 | /* ---- single-board build: rooms are tasks, status travels in a queue ---- */
 120 | 
 121 | 
 122 | bool room_status_post(const room_status_msg_t *m)
 123 | {
 124 |     return room_q != NULL && xQueueSend(room_q, m, 0) == pdTRUE;
 125 | }
 126 | 
 127 | void room_rx_task(void *arg)
 128 | {
 129 |     room_status_msg_t m;
 130 |     while (1)
 131 |         if (xQueueReceive(room_q, &m, portMAX_DELAY) == pdTRUE) room_status_apply(&m);
 132 | }
 133 | 
 134 | 
 135 | /* ------------------------------ LCD task ---------------------------- */
 136 | /* 16x2 LCD: the dashboard rotates through 2 pages (3 while an alarm is latched). */
 137 | static const char *room_label(const room_state_t *s)
 138 | {
 139 |     return !s->online ? "--" : !s->occupied ? "OFF" : s->light_on ? "ON" : "OCC";
 140 | }
 141 | 
 142 | void display_task(void *arg)
 143 | {
 144 |     for (unsigned tick = 0;; tick++) {
 145 |         struct tm t;
 146 |         room_state_t rs[3];
 147 |         char alarm_ts[16];
 148 | 
 149 |         ds3231_get_time(&t);
 150 |         LOCK(state_lock);
 151 |         int occ = inside_count, alarm = alarm_room;
 152 |         snprintf(alarm_ts, sizeof(alarm_ts), "%s", alarm_time);
 153 |         memcpy(rs, room_states, sizeof(rs));
 154 |         UNLOCK(state_lock);
 155 | 
 156 |         bool within = within_hours(t.tm_hour * 60 + t.tm_min);
 157 |         bool ovr = override_minutes_left() > 0;
 158 |         unsigned page = (tick / 6) % (alarm ? 3 : 2);
 159 | 
 160 |         LOCK(lcd_lock);
 161 |         if (page == 0) {
 162 |             lcd_printf(0, 0, "%02d:%02d:%02d IN:%-2d", t.tm_hour, t.tm_min, t.tm_sec, occ);
 163 |             lcd_printf(1, 0, "R1:%-3s R2:%-3s", room_label(&rs[1]), room_label(&rs[2]));
 164 |         } else if (page == 1) {
 165 |             lcd_printf(0, 0, "Hrs %02d:%02d-%02d:%02d", open_min / 60, open_min % 60,
 166 |                        close_min / 60, close_min % 60);
 167 |             lcd_printf(1, 0, "Door: %s", within ? "OPEN" : (ovr ? "EXIT ONLY" : "CLOSED"));
 168 |         } else {
 169 |             lcd_printf(0, 0, "!! INTRUDER !!");
 170 |             lcd_printf(1, 0, "R%d @%s", alarm, alarm_ts);
 171 |         }
 172 |         UNLOCK(lcd_lock);
 173 | 
 174 |         vTaskDelay(pdMS_TO_TICKS(500));
 175 |     }
 176 | }
 177 | 
 178 | 
 179 | void entrance_app_main(void)
 180 | {
 181 |     state_lock = xSemaphoreCreateMutex();
 182 |     sd_lock    = xSemaphoreCreateMutex();
 183 |     lcd_lock   = xSemaphoreCreateMutex();
 184 |     door_lock  = xSemaphoreCreateMutex();
 185 |     rfid_lock = xSemaphoreCreateMutex();
 186 |     if (!state_lock || !sd_lock || !lcd_lock || !door_lock || !rfid_lock) {
 187 |         ESP_LOGE(TAG, "Cannot allocate synchronization mutexes");
 188 |         abort();
 189 |     }
 190 | 
 191 |     load_config();
 192 |     occupancy_load();
 193 |     gpio_config_t out = {
 194 |         .pin_bit_mask = (1ULL << PIN_BUZZER) | (1ULL << PIN_LED_GREEN) | (1ULL << PIN_LED_RED),
 195 |         .mode         = GPIO_MODE_OUTPUT,
 196 |     };
 197 |     gpio_config(&out);
 198 | 
 199 |     ds3231_init();                       /* also installs the I2C driver the LCD uses */
 200 |     lcd_init(0x27);
 201 |     lcd_printf(0, 0, "CoCIS SMART SYS");
 202 |     lcd_printf(1, 0, "Booting...");
 203 | 
 204 |     /* both RC522 readers share one SPI bus (VSPI / SPI3) */
 205 |     spi_bus_config_t bus = {
 206 |         .mosi_io_num     = PIN_RC522_MOSI,
 207 |         .miso_io_num     = PIN_RC522_MISO,
 208 |         .sclk_io_num     = PIN_RC522_SCK,
 209 |         .quadwp_io_num   = -1,
 210 |         .quadhd_io_num   = -1,
 211 |         .max_transfer_sz = 64,
 212 |     };
 213 |     ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &bus, SPI_DMA_CH_AUTO));
 214 |     rc522_init(&reader[0], SPI3_HOST, PIN_RC522_IN_CS,  PIN_RC522_IN_RST);
 215 |     rc522_init(&reader[1], SPI3_HOST, PIN_RC522_OUT_CS, PIN_RC522_OUT_RST);
 216 | 
 217 |     servos_init();
 218 |     sd_init();
 219 |     tags_load();
 220 |     if (xTaskCreate(alarm_task, "alarm", 2048, NULL, 4, &alarm_task_handle) != pdPASS) abort();
 221 | 
 222 |     /* The rooms run as tasks on this ESP32 and report through a queue. */
 223 |     room_q = xQueueCreate(8, sizeof(room_status_msg_t));
 224 |     if (!room_q) { ESP_LOGE(TAG, "Cannot allocate room queue"); abort(); }
 225 |     if (xTaskCreate(room_rx_task, "room_rx", 4096, NULL, 4, NULL) != pdPASS) abort();
 226 |     room_start(1, PIN_ROOM1_PIR, PIN_ROOM1_LIGHT, -1);
 227 |     room_start(2, PIN_ROOM2_PIR, PIN_ROOM2_LIGHT, -1);
 228 |     ESP_LOGI(TAG, "Rooms 1 and 2 running as local tasks (FreeRTOS queue)");
 229 | 
 230 | 
 231 |     if (xTaskCreate(rfid_task,    "rfid",    4096, NULL, 5, NULL) != pdPASS) abort();
 232 |     if (xTaskCreate(display_task, "display", 4096, NULL, 3, NULL) != pdPASS) abort();
 233 |     if (xTaskCreate(menu_task,    "menu",    6144, NULL, 4, NULL) != pdPASS) abort();
 234 |     ESP_LOGI(TAG, "Entrance terminal ready. Default admin password: admin123");
 235 | }
 236 | 
 237 | 
```

### `access_control.c` — 290 lines
```c
   1 | /* RFID, registered tags, occupancy, door control and access decisions. */
   2 | #include "entrance_internal.h"
   3 | static const char *TAG = "entrance";
   4 | 
   5 | /* Seed list, used only when the SD card has no tags file yet. Wokwi MFRC522
   6 |  * preset cards: Blue 01:02:03:04, Green 11:22:33:44, Red AA:BB:CC:DD.
   7 |  * Yellow (55:66:77:88) and the Key Fob (C0:FF:EE:99) are deliberately NOT
   8 |  * registered, so they demonstrate "unknown tag". */
   9 | static const struct { uint8_t uid[4]; const char *name; } DEFAULT_TAGS[] = {
  10 |     {{0x01, 0x02, 0x03, 0x04}, "Ivan Nyayo"},   /* Blue Card  */
  11 |     {{0x11, 0x22, 0x33, 0x44}, "Jane Staff"},   /* Green Card */
  12 |     {{0xAA, 0xBB, 0xCC, 0xDD}, "John Admin"},   /* Red Card   */
  13 | };
  14 | 
  15 | /* ------------------- people tables (CALLER HOLDS state_lock) -------- */
  16 | int find_uid(const person_t *list, int n, const uint8_t uid[4])
  17 | {
  18 |     for (int i = 0; i < n; i++)
  19 |         if (memcmp(list[i].uid, uid, 4) == 0) return i;
  20 |     return -1;
  21 | }
  22 | 
  23 | void person_set(person_t *p, const uint8_t uid[4], const char *name, uint32_t entry_min)
  24 | {
  25 |     memcpy(p->uid, uid, 4);
  26 |     snprintf(p->name, sizeof(p->name), "%s", name);
  27 |     p->entry_min = entry_min;
  28 | }
  29 | 
  30 | void list_remove(person_t *list, int *n, int i)
  31 | {
  32 |     memmove(&list[i], &list[i + 1], (size_t)(*n - i - 1) * sizeof(person_t));
  33 |     (*n)--;
  34 | }
  35 | 
  36 | /* ------------------- tag registry on the SD card -------------------- */
  37 | /* Writes the whole registry to the SD card. CALLER HOLDS state_lock. */
  38 | void tags_write_file(void)
  39 | {
  40 |     if (!sdcard) { printf("(SD card not mounted - tag list not saved)\n"); return; }
  41 | 
  42 |     LOCK(sd_lock);
  43 |     FILE *f = fopen(TAGS_FILE_PATH, "w");
  44 |     if (f) {
  45 |         for (int i = 0; i < tag_count; i++)
  46 |             fprintf(f, UID_FMT ",%s\n", UID_ARGS(tag_table[i].uid), tag_table[i].name);
  47 |         fclose(f);
  48 |     } else {
  49 |         ESP_LOGW(TAG, "cannot open %s for writing", TAGS_FILE_PATH);
  50 |     }
  51 |     UNLOCK(sd_lock);
  52 | }
  53 | 
  54 | /* Boot only (no other task exists yet): reads the registry from the SD card
  55 |  * and seeds it with DEFAULT_TAGS the first time (no file yet). */
  56 | void tags_load(void)
  57 | {
  58 |     bool have_file = false;
  59 | 
  60 |     if (sdcard) {
  61 |         LOCK(sd_lock);
  62 |         FILE *f = fopen(TAGS_FILE_PATH, "r");
  63 |         if (f) {
  64 |             have_file = true;
  65 |             char line[64], name[24];
  66 |             uint8_t uid[4];
  67 |             while (tag_count < TABLE_MAX && fgets(line, sizeof(line), f)) {
  68 |                 const char *p = parse_uid(line, uid);
  69 |                 if (p && *p == ',' && sscanf(p + 1, "%23[^\r\n]", name) == 1)
  70 |                     person_set(&tag_table[tag_count++], uid, name, 0);
  71 |             }
  72 |             fclose(f);
  73 |         }
  74 |         UNLOCK(sd_lock);
  75 |     }
  76 | 
  77 |     if (!have_file) {
  78 |         for (size_t i = 0; i < sizeof(DEFAULT_TAGS) / sizeof(DEFAULT_TAGS[0]); i++)
  79 |             person_set(&tag_table[tag_count++], DEFAULT_TAGS[i].uid, DEFAULT_TAGS[i].name, 0);
  80 |         LOCK(state_lock);
  81 |         tags_write_file();
  82 |         UNLOCK(state_lock);
  83 |     }
  84 |     ESP_LOGI(TAG, "%d registered tag(s)", tag_count);
  85 | }
  86 | 
  87 | /* Occupancy snapshot is kept in NVS so a reset does not erase the roster.
  88 |  * Save only when a person enters/exits, not on every room heartbeat. */
  89 | void occupancy_save_locked(void)
  90 | {
  91 |     nvs_handle_t h;
  92 |     if (nvs_open("cocis_occ", NVS_READWRITE, &h) != ESP_OK) return;
  93 |     esp_err_t err = nvs_set_blob(h, "roster", inside_list,
  94 |                                   (size_t)inside_count * sizeof(person_t));
  95 |     if (err == ESP_OK) err = nvs_commit(h);
  96 |     nvs_close(h);
  97 |     if (err != ESP_OK) ESP_LOGE(TAG, "Failed to persist occupancy: %s", esp_err_to_name(err));
  98 | }
  99 | 
 100 | void occupancy_load(void)
 101 | {
 102 |     nvs_handle_t h;
 103 |     if (nvs_open("cocis_occ", NVS_READONLY, &h) != ESP_OK) return;
 104 |     size_t len = sizeof(inside_list);
 105 |     esp_err_t err = nvs_get_blob(h, "roster", inside_list, &len);
 106 |     nvs_close(h);
 107 |     if (err == ESP_OK && len % sizeof(person_t) == 0 && len <= sizeof(inside_list)) {
 108 |         inside_count = (int)(len / sizeof(person_t));
 109 |         ESP_LOGI(TAG, "Restored %d people inside from NVS", inside_count);
 110 |     } else if (err != ESP_ERR_NVS_NOT_FOUND) {
 111 |         ESP_LOGW(TAG, "Invalid occupancy snapshot; starting with empty roster");
 112 |     }
 113 | }
 114 | 
 115 | /* ------------------------- occupancy (mutex) ------------------------ */
 116 | bool lookup_tag(const uint8_t uid[4], char *name, size_t n)
 117 | {
 118 |     LOCK(state_lock);
 119 |     int i = find_uid(tag_table, tag_count, uid);
 120 |     if (i >= 0) snprintf(name, n, "%s", tag_table[i].name);
 121 |     UNLOCK(state_lock);
 122 |     return i >= 0;
 123 | }
 124 | 
 125 | bool is_inside(const uint8_t uid[4])
 126 | {
 127 |     LOCK(state_lock);
 128 |     int i = find_uid(inside_list, inside_count, uid);
 129 |     UNLOCK(state_lock);
 130 |     return i >= 0;
 131 | }
 132 | 
 133 | /* Returns false (and changes nothing) if the inside table is full. */
 134 | bool occupancy_enter(const uint8_t uid[4], const char *name, uint32_t entry_min)
 135 | {
 136 |     LOCK(state_lock);
 137 |     bool ok = inside_count < TABLE_MAX;
 138 |     if (ok) { person_set(&inside_list[inside_count++], uid, name, entry_min); occupancy_save_locked(); }
 139 |     UNLOCK(state_lock);
 140 |     return ok;
 141 | }
 142 | 
 143 | /* Returns false if the tag is not inside - the count never goes below 0. */
 144 | bool occupancy_leave(const uint8_t uid[4])
 145 | {
 146 |     LOCK(state_lock);
 147 |     int i = find_uid(inside_list, inside_count, uid);
 148 |     if (i >= 0) { list_remove(inside_list, &inside_count, i); occupancy_save_locked(); }
 149 |     UNLOCK(state_lock);
 150 |     return i >= 0;
 151 | }
 152 | 
 153 | /* Minutes left (rounded up) of an active admin exit override, or 0.
 154 |  * An expired override switches itself off here. */
 155 | int override_minutes_left(void)
 156 | {
 157 |     int left = 0;
 158 |     LOCK(state_lock);
 159 |     if (override_on) {
 160 |         int32_t ticks = (int32_t)(override_until - xTaskGetTickCount());
 161 |         if (ticks > 0) left = (int)((((uint32_t)ticks * portTICK_PERIOD_MS) + 59999) / 60000);
 162 |         else           override_on = false;
 163 |     }
 164 |     UNLOCK(state_lock);
 165 |     return left;
 166 | }
 167 | 
 168 | /* -------------------- servos, buzzer, indicator LEDs ---------------- */
 169 | void servo_angle(ledc_channel_t ch, int angle)
 170 | {
 171 |     /* 50 Hz, 14-bit: 0.5 ms..2.5 ms => duty 410..2048 */
 172 |     ledc_set_duty(LEDC_LOW_SPEED_MODE, ch, 410 + angle * (2048 - 410) / 180);
 173 |     ledc_update_duty(LEDC_LOW_SPEED_MODE, ch);
 174 | }
 175 | 
 176 | void servos_init(void)
 177 | {
 178 |     ledc_timer_config_t t = {
 179 |         .speed_mode      = LEDC_LOW_SPEED_MODE,
 180 |         .timer_num       = LEDC_TIMER_0,
 181 |         .duty_resolution = LEDC_TIMER_14_BIT,
 182 |         .freq_hz         = 50,
 183 |         .clk_cfg         = LEDC_AUTO_CLK,
 184 |     };
 185 |     ledc_timer_config(&t);
 186 | 
 187 |     const int pins[2] = {PIN_SERVO_IN, PIN_SERVO_OUT};   /* channel 0 = IN, 1 = OUT */
 188 |     for (int i = 0; i < 2; i++) {
 189 |         ledc_channel_config_t c = {
 190 |             .speed_mode = LEDC_LOW_SPEED_MODE,
 191 |             .channel    = (ledc_channel_t)i,
 192 |             .timer_sel  = LEDC_TIMER_0,
 193 |             .gpio_num   = pins[i],
 194 |         };
 195 |         ledc_channel_config(&c);
 196 |         servo_angle((ledc_channel_t)i, 0);
 197 |     }
 198 | }
 199 | 
 200 | void beep(int ms)
 201 | {
 202 |     gpio_set_level(PIN_BUZZER, 1);
 203 |     vTaskDelay(pdMS_TO_TICKS(ms));
 204 |     gpio_set_level(PIN_BUZZER, 0);
 205 | }
 206 | 
 207 | void blink(int gpio, int times, int ms)
 208 | {
 209 |     for (int i = 0; i < times; i++) {
 210 |         gpio_set_level(gpio, 1);
 211 |         vTaskDelay(pdMS_TO_TICKS(ms));
 212 |         gpio_set_level(gpio, 0);
 213 |         vTaskDelay(pdMS_TO_TICKS(ms));
 214 |     }
 215 | }
 216 | 
 217 | void grant_access(const uint8_t uid[4], const char *name, bool entering, const char *note)
 218 | {
 219 |     sd_log("%s,%s," UID_FMT ",%s", entering ? "ENTRY" : "EXIT", name, UID_ARGS(uid), note);
 220 |     printf("[ACCESS] GRANTED: %s (%s)\n", name, entering ? "ENTRY" : "EXIT");
 221 |     blink(PIN_LED_GREEN, 2, 100);
 222 |     beep(80);
 223 |     ledc_channel_t ch = entering ? LEDC_CHANNEL_0 : LEDC_CHANNEL_1;
 224 |     servo_angle(ch, 90);                         /* open  */
 225 |     vTaskDelay(pdMS_TO_TICKS(DOOR_OPEN_MS));
 226 |     servo_angle(ch, 0);                          /* close */
 227 | }
 228 | 
 229 | /* why = text for the SD log, msg = text for the serial console */
 230 | void deny_access(const uint8_t uid[4], const char *name, const char *why, const char *msg)
 231 | {
 232 |     sd_log("DENY,%s," UID_FMT ",%s", name, UID_ARGS(uid), why);
 233 |     printf("[ACCESS] DENIED: %s\n", msg);
 234 |     blink(PIN_LED_RED, 3, 100);
 235 |     beep(300);
 236 | }
 237 | 
 238 | /* --------------------------- access decision ------------------------ */
 239 | /* Called by the RFID task (real card) and by the serial "tap" command
 240 |  * (simulated card). door_lock lets only one decision / door movement run. */
 241 | void process_tag(const uint8_t uid[4], bool in_lane)
 242 | {
 243 |     LOCK(door_lock);
 244 |     printf("[SCAN] %s lane, tag " UID_FMT "\n", in_lane ? "IN" : "OUT", UID_ARGS(uid));
 245 | 
 246 |     char name[24];
 247 |     bool known = lookup_tag(uid, name, sizeof(name));
 248 |     int  now   = now_minutes();                        /* read the clock ONCE */
 249 |     bool within = within_hours(now);
 250 |     /* An admin can allow EXITS (never entries) outside the schedule so that
 251 |      * nobody is locked in after closing time. */
 252 |     bool exit_override = !in_lane && !within && override_minutes_left() > 0;
 253 | 
 254 |     if (!known) {
 255 |         deny_access(uid, "UNKNOWN_UID", "tag not registered", "unknown tag");
 256 |     } else if (!within && !exit_override) {
 257 |         deny_access(uid, name, "outside schedule", "outside access hours");
 258 |     } else if (in_lane) {
 259 |         if (is_inside(uid))
 260 |             deny_access(uid, name, "already inside", "tag already inside (possible tailgating)");
 261 |         else if (!occupancy_enter(uid, name, (uint32_t)now))
 262 |             deny_access(uid, name, "occupancy table full", "occupancy table full");
 263 |         else
 264 |             grant_access(uid, name, true, "allowed");
 265 |     } else {
 266 |         if (!occupancy_leave(uid))
 267 |             deny_access(uid, name, "not inside (no matching entry)", "no matching entry record");
 268 |         else
 269 |             grant_access(uid, name, false, exit_override ? "allowed (admin exit override)" : "allowed");
 270 |     }
 271 |     UNLOCK(door_lock);
 272 | }
 273 | 
 274 | void rfid_task(void *arg)
 275 | {
 276 |     while (1) {
 277 |         for (int lane = 0; lane < 2; lane++) {
 278 |             uint8_t uid[4];
 279 |             LOCK(rfid_lock);
 280 |             bool found = rc522_read_uid(&reader[lane], uid);
 281 |             UNLOCK(rfid_lock);
 282 |             if (found) {
 283 |                 process_tag(uid, lane == 0);
 284 |                 vTaskDelay(pdMS_TO_TICKS(ACCESS_COOLDOWN_MS));
 285 |             }
 286 |             vTaskDelay(pdMS_TO_TICKS(50));
 287 |         }
 288 |     }
 289 | }
 290 | 
```

### `system_manager.c` — 595 lines
```c
   1 | /* Persistent configuration, RTC helpers, SD logging and serial commands. */
   2 | #include "entrance_internal.h"
   3 | static const char *TAG = "entrance";
   4 | 
   5 | /* ============================== helpers ============================= */
   6 | void time_string(char *buf, size_t n)
   7 | {
   8 |     struct tm t;
   9 |     ds3231_get_time(&t);
  10 |     snprintf(buf, n, "%04d-%02d-%02d %02d:%02d:%02d", t.tm_year + 1900, t.tm_mon + 1,
  11 |              t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec);
  12 | }
  13 | 
  14 | int now_minutes(void)
  15 | {
  16 |     struct tm t;
  17 |     ds3231_get_time(&t);
  18 |     return t.tm_hour * 60 + t.tm_min;
  19 | }
  20 | 
  21 | bool within_hours(int minutes) { return minutes >= open_min && minutes <= close_min; }
  22 | 
  23 | bool require_auth(void)
  24 | {
  25 |     if (!authed) printf("DENIED: login first (login <password>)\n");
  26 |     return authed;
  27 | }
  28 | 
  29 | /* Parses 8 hex digits at the start of s into uid. Returns the position just
  30 |  * after them, or NULL if s does not start with a UID. */
  31 | const char *parse_uid(const char *s, uint8_t uid[4])
  32 | {
  33 |     unsigned u[4];
  34 |     int n = 0;
  35 |     if (sscanf(s, "%2x%2x%2x%2x%n", &u[0], &u[1], &u[2], &u[3], &n) != 4) return NULL;
  36 |     for (int i = 0; i < 4; i++) uid[i] = (uint8_t)u[i];
  37 |     return s + n;
  38 | }
  39 | 
  40 | /* ------------------------------ SD log ------------------------------ */
  41 | void sd_log(const char *fmt, ...)
  42 | {
  43 |     if (!sdcard) return;
  44 | 
  45 |     char line[192], ts[32];
  46 |     time_string(ts, sizeof(ts));
  47 |     va_list ap;
  48 |     va_start(ap, fmt);
  49 |     vsnprintf(line, sizeof(line), fmt, ap);
  50 |     va_end(ap);
  51 | 
  52 |     LOCK(sd_lock);
  53 |     FILE *f = fopen(LOG_FILE_PATH, "a");
  54 |     if (f) {
  55 |         fprintf(f, "%s,%s\n", ts, line);
  56 |         fclose(f);
  57 |     } else {
  58 |         ESP_LOGW(TAG, "cannot open %s for writing", LOG_FILE_PATH);
  59 |     }
  60 |     UNLOCK(sd_lock);
  61 | }
  62 | 
  63 | /* --------------------------- config (NVS) ---------------------------- */
  64 | void load_config(void)
  65 | {
  66 |     nvs_handle_t h;
  67 |     if (nvs_open("cocis", NVS_READONLY, &h) != ESP_OK) return;
  68 |     size_t len = sizeof(admin_pass);
  69 |     nvs_get_u16(h, "open_min", &open_min);
  70 |     nvs_get_u16(h, "close_min", &close_min);
  71 |     nvs_get_str(h, "admin_pass", admin_pass, &len);
  72 |     nvs_close(h);
  73 |     if (open_min >= 1440 || close_min >= 1440 || open_min >= close_min) {
  74 |         ESP_LOGW(TAG, "Invalid saved schedule; restoring 06:30-21:00");
  75 |         open_min = 390; close_min = 1260;
  76 |     }
  77 | }
  78 | 
  79 | void save_config(void)
  80 | {
  81 |     nvs_handle_t h;
  82 |     if (nvs_open("cocis", NVS_READWRITE, &h) != ESP_OK) return;
  83 |     nvs_set_u16(h, "open_min", open_min);
  84 |     nvs_set_u16(h, "close_min", close_min);
  85 |     nvs_set_str(h, "admin_pass", admin_pass);
  86 |     nvs_commit(h);
  87 |     nvs_close(h);
  88 | }
  89 | 
  90 | 
  91 | /* ============================ SERIAL MENU ============================ */
  92 | void cmd_help(char *args)
  93 | {
  94 |     printf("\n==== CoCIS Smart System - Main Menu ====\n"
  95 |            "  status              system overview\n"
  96 |            "  inside              who is inside the building\n"
  97 |            "  rooms               room occupancy / lighting status\n"
  98 |            "  log                 print access log from SD card\n"
  99 |            "  time                show RTC time\n"
 100 |            "  settime HH MM SS    set RTC time\n"
 101 |            "  setdate DD MM YY    set RTC date\n"
 102 |            "  schedule            show access schedule\n"
 103 |            "  setopen HH:MM       set opening time   (auth)\n"
 104 |            "  setclose HH:MM      set closing time   (auth)\n"
 105 |            "  login <password>    authenticate admin (3 wrong tries = 60 s lockout)\n"
 106 |            "  logout              end the admin session\n"
 107 |            "  energy              lighting energy saved by the occupancy control\n"
 108 |            "  energy reset        clear the energy counters        (auth)\n"
 109 |            "  override <min|off>  allow EXITS outside hours, 1-120 min (auth)\n"
 110 |            "  ack                 acknowledge an intruder alert    (auth)\n"
 111 |            "  passwd <new>        change admin password (auth)\n"
 112 |            "  tap in|out <UID>    simulate a card (test), e.g. tap in 01020304 (auth)\n"
 113 |            "  tags                list registered tags            (auth)\n"
 114 |            "  addtag <UID> <name> register a tag, e.g. addtag 55667788 Visitor (auth)\n"
 115 |            "  deltag <UID>        remove a tag                    (auth)\n"
 116 |            "  scan                print RFID UIDs for 15 s        (auth)\n"
 117 |            "  help                show this menu\n\n");
 118 | }
 119 | 
 120 | void cmd_status(char *args)
 121 | {
 122 |     struct tm t;
 123 |     room_state_t rs[3];
 124 |     char alarm_ts[16];
 125 | 
 126 |     ds3231_get_time(&t);
 127 |     int now = t.tm_hour * 60 + t.tm_min;
 128 |     LOCK(state_lock);
 129 |     int occ = inside_count, alarm = alarm_room, alarms = alarm_count;
 130 |     snprintf(alarm_ts, sizeof(alarm_ts), "%s", alarm_time);
 131 |     memcpy(rs, room_states, sizeof(rs));
 132 |     UNLOCK(state_lock);
 133 |     int ovr = override_minutes_left();
 134 | 
 135 |     printf("---- STATUS ----\n");
 136 |     printf("Build        : " BUILD_NAME "\n");
 137 |     printf("Clock source : %s\n", ds3231_available() ? "RTC chip (I2C 0x68)" : "software clock (no RTC found)");
 138 |     printf("RTC time     : %04d-%02d-%02d %02d:%02d:%02d\n",
 139 |            t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec);
 140 |     printf("Schedule     : %02d:%02d - %02d:%02d (now %02d:%02d) => %s\n",
 141 |            open_min / 60, open_min % 60, close_min / 60, close_min % 60,
 142 |            t.tm_hour, t.tm_min, within_hours(now) ? "OPEN" : "CLOSED");
 143 |     printf("Occupancy    : %d inside\n", occ);
 144 |     for (int r = 1; r <= 2; r++)
 145 |         printf("Room %d       : %s, light %s\n", r,
 146 |                rs[r].occupied ? "OCCUPIED" : "EMPTY", rs[r].light_on ? "ON" : "OFF");
 147 |     printf("SD card      : %s\n", sdcard ? "mounted" : "NOT mounted");
 148 |     printf("Admin auth   : %s\n", authed ? "yes" : "no");
 149 |     if (ovr > 0) printf("Exit override: ON (about %d min left)\n", ovr);
 150 |     else         printf("Exit override: off\n");
 151 |     if (alarm)
 152 |         printf("Intruder alarm: LATCHED - Room %d at %s (use 'ack'), %d alert(s) since boot\n",
 153 |                alarm, alarm_ts, alarms);
 154 |     else
 155 |         printf("Intruder alarm: none (%d alert(s) since boot)\n", alarms);
 156 | }
 157 | 
 158 | void cmd_inside(char *args)
 159 | {
 160 |     LOCK(state_lock);
 161 |     printf("---- INSIDE (%d) ----\n", inside_count);
 162 |     for (int i = 0; i < inside_count; i++)
 163 |         printf("  %-20s since %02u:%02u  uid:" UID_FMT "\n", inside_list[i].name,
 164 |                (unsigned)(inside_list[i].entry_min / 60), (unsigned)(inside_list[i].entry_min % 60),
 165 |                UID_ARGS(inside_list[i].uid));
 166 |     UNLOCK(state_lock);
 167 | }
 168 | 
 169 | void cmd_rooms(char *args)
 170 | {
 171 |     for (int r = 1; r <= 2; r++) {
 172 |         LOCK(state_lock);
 173 |         room_state_t s = room_states[r];
 174 |         UNLOCK(state_lock);
 175 |         printf("Room %d: %s | %s | motion events: %u\n", r, s.online ? "online" : "OFFLINE",
 176 |                s.occupied ? (s.light_on ? "OCCUPIED, light ON" : "OCCUPIED, light OFF")
 177 |                           : (s.light_on ? "EMPTY, light ON (!)" : "EMPTY, light OFF"),
 178 |                s.motion_events);
 179 |     }
 180 | }
 181 | 
 182 | void cmd_log(char *args)
 183 | {
 184 |     if (!sdcard) { printf("SD card not mounted.\n"); return; }
 185 | 
 186 |     LOCK(sd_lock);
 187 |     FILE *f = fopen(LOG_FILE_PATH, "r");
 188 |     if (f) {
 189 |         char line[160];
 190 |         while (fgets(line, sizeof(line), f)) printf("%s", line);
 191 |         fclose(f);
 192 |     } else {
 193 |         printf("Log file not found (nothing has been logged yet).\n");
 194 |     }
 195 |     UNLOCK(sd_lock);
 196 | }
 197 | 
 198 | void cmd_time(char *args)
 199 | {
 200 |     char ts[32];
 201 |     time_string(ts, sizeof(ts));
 202 |     printf("%s\n", ts);
 203 | }
 204 | 
 205 | void cmd_settime(char *args)
 206 | {
 207 |     int h, m, s;
 208 |     if (sscanf(args, "%d %d %d", &h, &m, &s) != 3 || h < 0 || h > 23 || m < 0 || m > 59 || s < 0 || s > 59) { printf("Usage: settime HH MM SS (valid 24-hour time)\n"); return; }
 209 |     struct tm t;
 210 |     ds3231_get_time(&t);
 211 |     t.tm_hour = h; t.tm_min = m; t.tm_sec = s;
 212 |     if (ds3231_set_time(&t) == ESP_OK) { sd_log("ADMIN,SET_TIME,-,-"); printf("RTC time set.\n"); }
 213 |     else printf("RTC time update failed.\n");
 214 | }
 215 | 
 216 | void cmd_setdate(char *args)
 217 | {
 218 |     int d, mo, y;
 219 |     if (sscanf(args, "%d %d %d", &d, &mo, &y) != 3 || y < 0 || y > 99 || mo < 1 || mo > 12 || d < 1 || d > 31) { printf("Usage: setdate DD MM YY\n"); return; }
 220 |     struct tm valid = { .tm_year = y + 100, .tm_mon = mo - 1, .tm_mday = d, .tm_hour = 12 };
 221 |     mktime(&valid);
 222 |     if (valid.tm_mday != d || valid.tm_mon != mo - 1 || valid.tm_year != y + 100) { printf("Invalid calendar date.\n"); return; }
 223 |     struct tm t;
 224 |     ds3231_get_time(&t);
 225 |     t.tm_mday = d; t.tm_mon = mo - 1; t.tm_year = y + 100;
 226 |     if (ds3231_set_time(&t) == ESP_OK) { sd_log("ADMIN,SET_DATE,-,-"); printf("RTC date set.\n"); }
 227 |     else printf("RTC date update failed.\n");
 228 | }
 229 | 
 230 | void cmd_schedule(char *args)
 231 | {
 232 |     printf("Access schedule: %02d:%02d - %02d:%02d\n",
 233 |            open_min / 60, open_min % 60, close_min / 60, close_min % 60);
 234 | }
 235 | 
 236 | void set_schedule(const char *args, uint16_t *field)
 237 | {
 238 |     int h, m;
 239 |     const char *what = (field == &open_min) ? "open" : "close";
 240 |     char extra;
 241 |     if (sscanf(args, "%d:%d %c", &h, &m, &extra) != 2 || h < 0 || h > 23 || m < 0 || m > 59) {
 242 |         printf("Usage: set%s HH:MM (00:00 to 23:59)\n", what); return;
 243 |     }
 244 |     int candidate = h * 60 + m;
 245 |     if ((field == &open_min && candidate >= close_min) ||
 246 |         (field == &close_min && candidate <= open_min)) {
 247 |         printf("Opening must be earlier than closing (same-day schedule).\n"); return;
 248 |     }
 249 |     *field = (uint16_t)candidate;
 250 |     save_config();
 251 |     printf("%s time set to %02d:%02d\n", (field == &open_min) ? "Opening" : "Closing", h, m);
 252 | }
 253 | void cmd_setopen(char *args)  { set_schedule(args, &open_min); }
 254 | void cmd_setclose(char *args) { set_schedule(args, &close_min); }
 255 | 
 256 | void cmd_login(char *args)
 257 | {
 258 |     TickType_t now = xTaskGetTickCount();
 259 | 
 260 |     if (login_fails >= LOGIN_MAX_FAILS) {
 261 |         int32_t left = (int32_t)(login_locked_until - now);
 262 |         if (left > 0) {
 263 |             printf("Login locked after too many wrong passwords. Try again in %d s.\n",
 264 |                    (int)(((uint32_t)left * portTICK_PERIOD_MS) / 1000) + 1);
 265 |             return;
 266 |         }
 267 |         login_fails = 0;                                 /* lockout is over */
 268 |     }
 269 | 
 270 |     if (strcmp(args, admin_pass) == 0) {
 271 |         authed = true;
 272 |         login_fails = 0;
 273 |         sd_log("ADMIN,LOGIN_OK,-,-");
 274 |         printf("Authenticated.\n");
 275 |         return;
 276 |     }
 277 | 
 278 |     authed = false;
 279 |     login_fails++;
 280 |     sd_log("ADMIN,LOGIN_FAILED,attempt %d,-", login_fails);
 281 |     if (login_fails >= LOGIN_MAX_FAILS) {
 282 |         login_locked_until = now + pdMS_TO_TICKS(LOGIN_LOCK_MS);
 283 |         sd_log("ADMIN,LOGIN_LOCKOUT,%d s,-", LOGIN_LOCK_MS / 1000);
 284 |         printf("Wrong password. Login locked for %d s.\n", LOGIN_LOCK_MS / 1000);
 285 |     } else {
 286 |         printf("Wrong password (%d of %d attempts).\n", login_fails, LOGIN_MAX_FAILS);
 287 |     }
 288 | }
 289 | 
 290 | void cmd_logout(char *args)
 291 | {
 292 |     authed = false;
 293 |     printf("Logged out.\n");
 294 | }
 295 | 
 296 | void cmd_passwd(char *args)
 297 | {
 298 |     size_t n = strlen(args);
 299 |     if (n == 0 || n >= sizeof(admin_pass)) {
 300 |         printf("Password must be 1-%d characters.\n", (int)sizeof(admin_pass) - 1);
 301 |         return;
 302 |     }
 303 |     memcpy(admin_pass, args, n + 1);
 304 |     save_config();
 305 |     printf("Password changed.\n");
 306 | }
 307 | 
 308 | /* --------------------------- energy report -------------------------- */
 309 | void fmt_hms(char *buf, size_t n, uint32_t ms)
 310 | {
 311 |     uint32_t s = ms / 1000;
 312 |     snprintf(buf, n, "%02u : %02u : %02u", (unsigned)(s / 3600), (unsigned)((s / 60) % 60), (unsigned)(s % 60));
 313 | }
 314 | 
 315 | /* Energy saved by switching lights off in empty rooms, compared with the
 316 |  * lights simply staying on for the whole time the room was monitored. */
 317 | void cmd_energy(char *args)
 318 | {
 319 |     room_state_t snap[3];
 320 | 
 321 |     if (!strcmp(args, "reset")) {
 322 |         if (!require_auth()) return;
 323 |         LOCK(state_lock);
 324 |         for (int r = 1; r <= 2; r++) {
 325 |             room_state_t *s = &room_states[r];
 326 |             s->monitored_ms = s->light_ms = s->occ_ms = 0;
 327 |             s->acct_tick = xTaskGetTickCount();
 328 |         }
 329 |         UNLOCK(state_lock);
 330 |         sd_log("ADMIN,ENERGY_RESET,-,-");
 331 |         printf("Energy counters cleared.\n");
 332 |         return;
 333 |     }
 334 | 
 335 |     LOCK(state_lock);
 336 |     for (int r = 1; r <= 2; r++) {
 337 |         room_account(&room_states[r], xTaskGetTickCount());   /* credit up to this moment */
 338 |         snap[r] = room_states[r];
 339 |     }
 340 |     UNLOCK(state_lock);
 341 | 
 342 |     printf("---- ENERGY REPORT (assumed %d W per room light) ----\n", LIGHT_WATTS);
 343 |     uint32_t tot_mon = 0, tot_saved = 0;
 344 |     for (int r = 1; r <= 2; r++) {
 345 |         const room_state_t *s = &snap[r];
 346 |         if (!s->online) { printf("Room %d: no data yet (room offline)\n", r); continue; }
 347 | 
 348 |         uint32_t saved = (s->monitored_ms > s->light_ms) ? s->monitored_ms - s->light_ms : 0;
 349 |         unsigned pct = s->monitored_ms ? (unsigned)(((uint64_t)saved * 100) / s->monitored_ms) : 0;
 350 |         unsigned long long mwh = (unsigned long long)saved * LIGHT_WATTS / 3600;
 351 |         char a[16], b[16], c[16], d[16];
 352 |         fmt_hms(a, sizeof(a), s->monitored_ms);
 353 |         fmt_hms(b, sizeof(b), s->light_ms);
 354 |         fmt_hms(c, sizeof(c), s->occ_ms);
 355 |         fmt_hms(d, sizeof(d), saved);
 356 |         printf("Room %d: monitored %s | light on %s | occupied %s\n", r, a, b, c);
 357 |         printf("        light off for %s = %u%% saved, about %llu.%03llu Wh\n",
 358 |                d, pct, mwh / 1000, mwh % 1000);
 359 |         tot_mon += s->monitored_ms;
 360 |         tot_saved += saved;
 361 |     }
 362 |     unsigned long long tot_mwh = (unsigned long long)tot_saved * LIGHT_WATTS / 3600;
 363 |     printf("-- TOTAL : about %llu.%03llu Wh saved over %u%% of the monitored time\n",
 364 |            tot_mwh / 1000, tot_mwh % 1000,
 365 |            tot_mon ? (unsigned)(((uint64_t)tot_saved * 100) / tot_mon) : 0);
 366 |     printf("(baseline = light left on the whole time; counters restart at reboot)\n");
 367 | }
 368 | 
 369 | /* ---------------------- alarm / override / tap ---------------------- */
 370 | void cmd_ack(char *args)
 371 | {
 372 |     LOCK(state_lock);
 373 |     int room = alarm_room;
 374 |     alarm_room = 0;
 375 |     UNLOCK(state_lock);
 376 | 
 377 |     if (room) {
 378 |         sd_log("ADMIN,ALARM_ACK,Room %d,-", room);
 379 |         printf("Intruder alarm for Room %d acknowledged.\n", room);
 380 |     } else {
 381 |         printf("No alarm to acknowledge.\n");
 382 |     }
 383 | }
 384 | 
 385 | void cmd_override(char *args)
 386 | {
 387 |     int minutes = 0;
 388 |     bool off = !strcmp(args, "off");
 389 | 
 390 |     if (!off && !(sscanf(args, "%d", &minutes) == 1 && minutes >= 1 && minutes <= 120)) {
 391 |         printf("Usage: override <1-120 minutes> | override off\n"
 392 |                "Allows EXITS outside the schedule, but not ENTRIES.\n");
 393 |         return;
 394 |     }
 395 | 
 396 |     LOCK(state_lock);
 397 |     override_on = !off;
 398 |     if (!off) override_until = xTaskGetTickCount() + pdMS_TO_TICKS((uint32_t)minutes * 60000);
 399 |     UNLOCK(state_lock);
 400 | 
 401 |     if (off) {
 402 |         sd_log("ADMIN,EXIT_OVERRIDE_OFF,-,-");
 403 |         printf("Exit override switched off.\n");
 404 |     } else {
 405 |         sd_log("ADMIN,EXIT_OVERRIDE_ON,%d min,-", minutes);
 406 |         printf("Exits allowed outside hours for %d min (entries stay blocked).\n", minutes);
 407 |     }
 408 | }
 409 | 
 410 | void cmd_tap(char *args)
 411 | {
 412 |     char lane[4];
 413 |     uint8_t uid[4];
 414 |     int n = 0;
 415 | 
 416 |     if (sscanf(args, "%3s%n", lane, &n) == 1 && (!strcmp(lane, "in") || !strcmp(lane, "out")) &&
 417 |         parse_uid(args + n, uid))
 418 |         process_tag(uid, lane[0] == 'i');
 419 |     else
 420 |         printf("Usage: tap in|out <8 hex digits>   e.g. tap in 01020304\n");
 421 | }
 422 | 
 423 | /* --------------------------- tag management ------------------------- */
 424 | void cmd_tags(char *args)
 425 | {
 426 |     LOCK(state_lock);
 427 |     printf("---- REGISTERED TAGS (%d/%d) ----\n", tag_count, TABLE_MAX);
 428 |     for (int i = 0; i < tag_count; i++)
 429 |         printf("  " UID_FMT "  %s\n", UID_ARGS(tag_table[i].uid), tag_table[i].name);
 430 |     UNLOCK(state_lock);
 431 | }
 432 | 
 433 | void cmd_addtag(char *args)
 434 | {
 435 |     uint8_t uid[4];
 436 |     char name[24], existing[24];
 437 |     const char *p = parse_uid(args, uid);
 438 | 
 439 |     if (!p || sscanf(p, " %23[^\r\n]", name) != 1 || strchr(name, ',')) {
 440 |         printf("Usage: addtag <8 hex digits> <name>   e.g. addtag 55667788 Visitor (no commas)\n");
 441 |         return;
 442 |     }
 443 |     if (lookup_tag(uid, existing, sizeof(existing))) {
 444 |         printf("Tag already registered to %s\n", existing);
 445 |         return;
 446 |     }
 447 | 
 448 |     LOCK(state_lock);
 449 |     bool full = tag_count >= TABLE_MAX;
 450 |     if (!full) {
 451 |         person_set(&tag_table[tag_count++], uid, name, 0);
 452 |         tags_write_file();
 453 |     }
 454 |     UNLOCK(state_lock);
 455 | 
 456 |     if (full) { printf("Tag table full (%d tags).\n", TABLE_MAX); return; }
 457 |     sd_log("ADMIN,TAG_ADDED," UID_FMT ",%s", UID_ARGS(uid), name);
 458 |     printf("Registered " UID_FMT " as %s\n", UID_ARGS(uid), name);
 459 | }
 460 | 
 461 | void cmd_deltag(char *args)
 462 | {
 463 |     uint8_t uid[4];
 464 | 
 465 |     if (!parse_uid(args, uid)) { printf("Usage: deltag <8 hex digits>\n"); return; }
 466 |     if (is_inside(uid)) {
 467 |         printf("That tag is currently inside the building - let it exit first.\n");
 468 |         return;
 469 |     }
 470 | 
 471 |     LOCK(state_lock);
 472 |     int i = find_uid(tag_table, tag_count, uid);
 473 |     if (i >= 0) {
 474 |         list_remove(tag_table, &tag_count, i);
 475 |         tags_write_file();
 476 |     }
 477 |     UNLOCK(state_lock);
 478 | 
 479 |     if (i < 0) { printf("No such tag.\n"); return; }
 480 |     sd_log("ADMIN,TAG_REMOVED," UID_FMT ",-", UID_ARGS(uid));
 481 |     printf("Removed tag " UID_FMT "\n", UID_ARGS(uid));
 482 | }
 483 | 
 484 | void cmd_scan(char *args)
 485 | {
 486 |     printf("Scanning for 15 seconds - present tags to either reader...\n");
 487 |     TickType_t end = xTaskGetTickCount() + pdMS_TO_TICKS(15000);
 488 |     while (xTaskGetTickCount() < end) {
 489 |         uint8_t uid[4];
 490 |         LOCK(rfid_lock);
 491 |         bool found = rc522_read_uid(&reader[0], uid) || rc522_read_uid(&reader[1], uid);
 492 |         UNLOCK(rfid_lock);
 493 |         if (found) printf("  UID: " UID_FMT "\n", UID_ARGS(uid));
 494 |         vTaskDelay(pdMS_TO_TICKS(300));
 495 |     }
 496 | }
 497 | 
 498 | /* ---------------------------- command table ------------------------- */
 499 | typedef struct {
 500 |     const char *name;
 501 |     void      (*fn)(char *args);
 502 |     bool        needs_auth;
 503 | } command_t;
 504 | 
 505 | static const command_t COMMANDS[] = {
 506 |     {"help",     cmd_help,     false}, {"status",  cmd_status,   false},
 507 |     {"inside",   cmd_inside,   false}, {"rooms",   cmd_rooms,    false},
 508 |     {"log",      cmd_log,      false}, {"time",    cmd_time,     false},
 509 |     {"settime",  cmd_settime,  true}, {"setdate", cmd_setdate,  true},
 510 |     {"schedule", cmd_schedule, false}, {"login",   cmd_login,    false},
 511 |     {"logout",   cmd_logout,   false}, {"energy",  cmd_energy,   false},  /* "energy reset" checks auth itself */
 512 |     {"setopen",  cmd_setopen,  true},  {"setclose", cmd_setclose, true},
 513 |     {"override", cmd_override, true},  {"ack",     cmd_ack,      true},
 514 |     {"passwd",   cmd_passwd,   true},  {"tap",     cmd_tap,      true},
 515 |     {"tags",     cmd_tags,     true},  {"addtag",  cmd_addtag,   true},
 516 |     {"deltag",   cmd_deltag,   true},  {"scan",    cmd_scan,     true},
 517 | };
 518 | 
 519 | /* Splits "name args" at the first space and runs the matching command. */
 520 | void run_command(char *line)
 521 | {
 522 |     char *args = strchr(line, ' ');
 523 |     if (args) *args++ = '\0';
 524 |     else      args = line + strlen(line);          /* empty argument string */
 525 | 
 526 |     for (size_t i = 0; i < sizeof(COMMANDS) / sizeof(COMMANDS[0]); i++) {
 527 |         if (strcmp(line, COMMANDS[i].name) != 0) continue;
 528 |         if (COMMANDS[i].needs_auth && !require_auth()) return;
 529 |         COMMANDS[i].fn(args);
 530 |         return;
 531 |     }
 532 |     printf("Unknown command. Type 'help'.\n");
 533 | }
 534 | 
 535 | void menu_task(void *arg)
 536 | {
 537 |     char line[96] = {0};
 538 |     size_t len = 0;
 539 | 
 540 |     cmd_help(NULL);
 541 |     while (1) {
 542 |         int c = getchar();
 543 |         if (c == EOF) { vTaskDelay(pdMS_TO_TICKS(20)); continue; }
 544 | 
 545 |         if (c == 0x08 || c == 0x7F) {                      /* backspace */
 546 |             if (len > 0) { line[--len] = '\0'; printf("\b \b"); }
 547 |         } else if (c != '\n' && c != '\r') {
 548 |             putchar(c);                                    /* local echo */
 549 |             if (len < sizeof(line) - 1) { line[len++] = (char)c; line[len] = '\0'; }
 550 |         } else {
 551 |             putchar('\n');
 552 |             if (len > 0) run_command(line);
 553 |             len = 0;
 554 |             line[0] = '\0';
 555 |         }
 556 |     }
 557 | }
 558 | 
 559 | /* ------------------------------- init ------------------------------- */
 560 | void sd_init(void)
 561 | {
 562 |     spi_bus_config_t bus = {
 563 |         .mosi_io_num     = PIN_SD_MOSI,
 564 |         .miso_io_num     = PIN_SD_MISO,
 565 |         .sclk_io_num     = PIN_SD_SCK,
 566 |         .quadwp_io_num   = -1,
 567 |         .quadhd_io_num   = -1,
 568 |         .max_transfer_sz = 4000,
 569 |     };
 570 |     esp_err_t ret = spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO);
 571 |     if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
 572 |         ESP_LOGE(TAG, "SD SPI bus init failed: %s", esp_err_to_name(ret));
 573 |         return;
 574 |     }
 575 | 
 576 |     sdmmc_host_t host = SDSPI_HOST_DEFAULT();
 577 |     host.slot = SPI2_HOST;
 578 |     sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
 579 |     slot.gpio_cs  = PIN_SD_CS;
 580 |     slot.host_id  = SPI2_HOST;
 581 |     esp_vfs_fat_sdmmc_mount_config_t mount = {
 582 |         .format_if_mount_failed = false,
 583 |         .max_files              = 5,
 584 |         .allocation_unit_size   = 16 * 1024,
 585 |     };
 586 | 
 587 |     ret = esp_vfs_fat_sdspi_mount("/sdcard", &host, &slot, &mount, &sdcard);
 588 |     if (ret != ESP_OK) {
 589 |         ESP_LOGE(TAG, "SD mount failed: %s", esp_err_to_name(ret));
 590 |         sdcard = NULL;
 591 |         return;
 592 |     }
 593 |     sdmmc_card_print_info(stdout, sdcard);
 594 | }
 595 | 
```

### `CMakeLists.txt` — 5 lines
```cmake
   1 | idf_component_register(
   2 |     SRCS "main.c" "rc522.c" "ds3231.c" "lcd1602_i2c.c" "entrance_main.c" "access_control.c" "system_manager.c" "room_main.c"
   3 |     INCLUDE_DIRS "."
   4 |     REQUIRES nvs_flash driver fatfs sdmmc esp_timer
   5 | )
```

## 5. Guided execution traces
### 5.1 Successful entry
1. `rfid_task()` polls the entry reader. 2. `process_tag(uid, true)` acquires `door_lock`. 3. `lookup_tag()` checks the tag registry under `state_lock`. 4. `now_minutes()` reads RTC and `within_hours()` checks the schedule. 5. `is_inside()` rejects a duplicate entry. 6. `occupancy_enter()` adds the person and commits the NVS roster. 7. `grant_access()` logs the event, blinks green, sounds the buzzer, opens the entry servo for `DOOR_OPEN_MS` and closes it. 8. `door_lock` is released.
### 5.2 Denied entry
Unknown UID, outside-hours attempt, duplicate entry, or full roster leads to `deny_access()`: write a DENY log, print a reason, flash red and beep. The door servo remains closed.
### 5.3 Exit outside hours
Normal exit requires the tag to be registered and present in the occupancy roster. Outside the schedule, the admin may enable a time-limited **exit-only** override. It does not authorize entry. Emergency physical egress must be provided separately in a real building.
### 5.4 Motion in a lecture room
The room task (in unchanged `room_main.c`) detects PIR motion, switches the light on, and posts a `room_status_msg_t` to `room_q`. `room_rx_task()` receives it. `room_status_apply()` updates shared room state and energy counters. If the building occupancy roster is empty at the transition into occupied, `alert_intruder()` latches an alarm, writes a log and notifies `alarm_task()` to flash/sound.
### 5.5 Restart persistence
`main.c` initializes NVS; `entrance_app_main()` calls `load_config()` and `occupancy_load()`. Registered tags are loaded from the SD CSV via `tags_load()`. Persistence depends on the simulator/board retaining the corresponding flash and SD contents.

## 6. Demonstration and test checklist
- [ ] Build with `pio run -e standalone` and verify no undefined symbols or duplicate `app_main`.
- [ ] Scan a registered card at entry; confirm green indication, entry servo, occupancy count and log.
- [ ] Scan the same card twice at entry; confirm duplicate-entry denial.
- [ ] Scan an unknown card; confirm red indication and denial log.
- [ ] Exit with a registered inside tag; confirm exit servo and occupancy decrement.
- [ ] Try exit without a matching entry; confirm denial.
- [ ] Set RTC time within/outside schedule and verify access decisions.
- [ ] Authenticate, modify schedule, log out and verify protected commands.
- [ ] Trigger Room 1 and Room 2 PIR independently; verify lights and 30-second idle timeout.
- [ ] Trigger PIR while nobody is registered inside; verify latched alarm and admin `ack`.
- [ ] Check `energy`, `rooms`, `inside`, `status`, `tags`, and `log` commands.
- [ ] Restart the simulation and verify which NVS/SD data actually persists.

## 7. Questions to prepare for the presentation
**Q: Why is `app_main()` in ESP-IDF rather than `setup()`?**  
A: ESP-IDF provides the application entry point and runs it after system initialization; Arduino supplies its own entry point, which caused the earlier duplicate-definition error.

**Q: Why use one ESP32 for three logical terminals?**  
A: Independent FreeRTOS tasks represent entrance, Room 1 and Room 2 while sharing one microcontroller and local queue.

**Q: Why use mutexes?**  
A: They protect shared tables and peripheral operations from simultaneous task access.

**Q: What happens if a room queue fills?**  
A: `xQueueSend(..., 0)` can fail and drop a status update; later heartbeats can refresh state.

**Q: What is the difference between NVS and SD?**  
A: NVS stores small persistent key/value records and blobs; SD stores larger human-readable CSV logs and tag lists.

**Q: Why do RFID readers need different CS pins?**  
A: They share SPI clock/data but each chip-select enables one reader at a time.

**Q: How is lighting energy estimated?**  
A: The system accumulates elapsed light-on and occupied durations and applies the configured lamp wattage.

**Q: What is an important limitation?**  
A: PIR senses motion rather than exact occupancy; simulated security must not replace real emergency egress.

**Q: What is the default password?**  
A: The source initializes `admin_pass` to `admin123` unless NVS overrides it; this is for demonstration only and must be changed for real deployment.

**Q: Why does the LCD take a state snapshot?**  
A: It minimizes time holding `state_lock` and avoids reading partially updated shared data.

## 8. Engineering observations worth mentioning
- The default admin password is hard-coded and stored as plaintext in NVS. This is acceptable only as a coursework demo, not production-grade security.
- The room PIR inactivity timeout is an approximation of occupancy.
- `room_q` messages can be dropped when the queue is full; consider logging queue-send failures in future work.
- Time and date input should be validated and privileged operations should be checked at dispatch and/or handler boundaries.
- CSV names are bounded to the `person_t` name buffer, but real deployments should consider escaping commas and newlines.
- An access-control system must never prevent emergency evacuation.
- The included header exposes many globals for a small coursework system; for larger projects, encapsulated APIs would reduce coupling.

## 9. Source provenance
This document was generated from the actual ZIP archive of the **five changed modular files** in this conversation. The line-numbered listings are provided for direct comparison with VS Code. The source code is authoritative where explanatory prose summarizes its behavior.

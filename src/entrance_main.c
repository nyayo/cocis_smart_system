#include "entrance_internal.h"
static const char *TAG = "entrance";

/* ============================== state =============================== */
SemaphoreHandle_t state_lock, sd_lock, lcd_lock, door_lock;

person_t inside_list[TABLE_MAX];   /* people inside (count == occupancy) */
person_t tag_table[TABLE_MAX];     /* registered tags, mirrored on the SD card */
int inside_count, tag_count;


room_state_t room_states[3];       /* index 1..2 = rooms 1..2 */

volatile int alarm_room;           /* 0 = none, else the room that raised it */
char     alarm_time[16];
int      alarm_count;
TaskHandle_t alarm_task_handle;

bool       override_on;            /* admin exit override */
TickType_t override_until;

int        login_fails;            /* menu task only */
TickType_t login_locked_until;
bool       authed;

uint16_t open_min = 6 * 60 + 30, close_min = 21 * 60;   /* schedule, kept in NVS */
char     admin_pass[32] = "admin123";

SemaphoreHandle_t rfid_lock;
rc522_t      reader[2];            /* [0] = IN lane, [1] = OUT lane */
sdmmc_card_t *sdcard;

QueueHandle_t room_q;

/* ------------------------ intruder alarm ---------------------------- */
/* Beeps / flashes the red LED for ~10 s. The alarm itself stays LATCHED (shown
 * on the LCD) until an admin runs "ack". door_lock is taken for each beep so
 * the alarm never fights the door logic over the same LED / buzzer pins. */
void alarm_task(void *arg)
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
void alert_intruder(int room)
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
void room_account(room_state_t *s, uint32_t now_tick)
{
    if (!s->online) return;
    uint32_t dt = (now_tick - s->acct_tick) * portTICK_PERIOD_MS;
    s->acct_tick = now_tick;
    s->monitored_ms += dt;
    if (s->light_on) s->light_ms += dt;
    if (s->occupied) s->occ_ms   += dt;
}

/* ------------------------ room status intake ------------------------ */
/* Validate room task messages before updating shared state. */
void room_status_apply(const room_status_msg_t *m)
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

/* ---- single-board build: rooms are tasks, status travels in a queue ---- */


bool room_status_post(const room_status_msg_t *m)
{
    return room_q != NULL && xQueueSend(room_q, m, 0) == pdTRUE;
}

void room_rx_task(void *arg)
{
    room_status_msg_t m;
    while (1)
        if (xQueueReceive(room_q, &m, portMAX_DELAY) == pdTRUE) room_status_apply(&m);
}


/* ------------------------------ LCD task ---------------------------- */
/* 16x2 LCD: the dashboard rotates through 2 pages (3 while an alarm is latched). */
static const char *room_label(const room_state_t *s)
{
    return !s->online ? "--" : !s->occupied ? "OFF" : s->light_on ? "ON" : "OCC";
}

void display_task(void *arg)
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


void entrance_app_main(void)
{
    state_lock = xSemaphoreCreateMutex();
    sd_lock    = xSemaphoreCreateMutex();
    lcd_lock   = xSemaphoreCreateMutex();
    door_lock  = xSemaphoreCreateMutex();
    rfid_lock = xSemaphoreCreateMutex();
    if (!state_lock || !sd_lock || !lcd_lock || !door_lock || !rfid_lock) {
        ESP_LOGE(TAG, "Cannot allocate synchronization mutexes");
        abort();
    }

    load_config();
    occupancy_load();
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
    if (xTaskCreate(alarm_task, "alarm", 2048, NULL, 4, &alarm_task_handle) != pdPASS) abort();

    /* The rooms run as tasks on this ESP32 and report through a queue. */
    room_q = xQueueCreate(8, sizeof(room_status_msg_t));
    if (!room_q) { ESP_LOGE(TAG, "Cannot allocate room queue"); abort(); }
    if (xTaskCreate(room_rx_task, "room_rx", 4096, NULL, 4, NULL) != pdPASS) abort();
    room_start(1, PIN_ROOM1_PIR, PIN_ROOM1_LIGHT, -1);
    room_start(2, PIN_ROOM2_PIR, PIN_ROOM2_LIGHT, -1);
    ESP_LOGI(TAG, "Rooms 1 and 2 running as local tasks (FreeRTOS queue)");


    if (xTaskCreate(rfid_task,    "rfid",    4096, NULL, 5, NULL) != pdPASS) abort();
    if (xTaskCreate(display_task, "display", 4096, NULL, 3, NULL) != pdPASS) abort();
    if (xTaskCreate(menu_task,    "menu",    6144, NULL, 4, NULL) != pdPASS) abort();
    ESP_LOGI(TAG, "Entrance terminal ready. Default admin password: admin123");
}



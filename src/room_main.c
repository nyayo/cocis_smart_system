/*
 * room_main.c - CoCIS Smart System ROOM controller (Room 1 / Room 2)
 *
 *  - PIR motion => occupancy state
 *  - Automatic lighting: ON when occupied, OFF ROOM_EMPTY_TIMEOUT_S after the
 *    last motion
 *  - Pushes its status to the entrance terminal (on change + every 10 s as a
 *    heartbeat), so the entrance stays the single SD-card writer.
 *
 * Two builds share this file:
 *   -D STANDALONE                        both rooms run as tasks on the same
 *                                        ESP32; status goes through a queue
 *   -D TERMINAL_ROOM -D ROOM_ID=<1|2>    one room per ESP32; status goes to
 *                                        the entrance over ESP-NOW
 */
#if defined(TERMINAL_ROOM) || defined(STANDALONE)

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "common.h"

#ifndef STANDALONE
#include "esp_wifi.h"
#include "esp_now.h"
#endif

static const char *TAG = "room";

#ifndef ROOM_ID
#define ROOM_ID 1
#endif

#define HEARTBEAT_MS 10000

typedef struct {
    int        id;
    int        pir_pin;
    int        light_pin;
    int        status_pin;          /* < 0 = no status LED */
    uint32_t   motion_events;
    TickType_t last_motion_tick;
    bool       occupied;
    bool       light_on;
} room_ctx_t;

static room_ctx_t room_ctx[3];      /* indexed by room id (1..2) */

static void status_led_blink(const room_ctx_t *r, int ms)
{
    if (r->status_pin < 0) return;
    gpio_set_level(r->status_pin, 1);
    vTaskDelay(pdMS_TO_TICKS(ms));
    gpio_set_level(r->status_pin, 0);
}

static void send_status(const room_ctx_t *r)
{
    room_status_msg_t m = {
        .magic         = COCIS_MAGIC,
        .msg_type      = MSG_ROOM_STATUS,
        .room_id       = (uint8_t)r->id,
        .occupied      = r->occupied,
        .light_on      = r->light_on,
        .motion_events = (uint16_t)r->motion_events,
        .uptime_sec    = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS / 1000),
    };

#ifdef STANDALONE
    bool ok = room_status_post(&m);
#else
    uint8_t dst[] = ENTRANCE_MAC_ADDR;
    bool ok = (esp_now_send(dst, (uint8_t *)&m, sizeof(m)) == ESP_OK);
#endif

    if (ok) status_led_blink(r, 60);
    else    ESP_LOGW(TAG, "room %d: status delivery failed", r->id);
}

/* Occupied <-> empty transition: the light follows the occupancy. */
static void set_occupied(room_ctx_t *r, bool occupied)
{
    r->occupied = r->light_on = occupied;
    if (occupied) r->motion_events++;
    gpio_set_level(r->light_pin, occupied);
    printf("[ROOM%d] %s -> light %s\n", r->id,
           occupied ? "occupied" : "empty", occupied ? "ON" : "OFF");
    send_status(r);
}

static void room_task(void *arg)
{
    room_ctx_t *r = (room_ctx_t *)arg;
    TickType_t last_heartbeat = 0;

    status_led_blink(r, 200);
    send_status(r);                                  /* announce ourselves */

    while (1) {
        TickType_t now = xTaskGetTickCount();

        if (gpio_get_level(r->pir_pin)) {
            r->last_motion_tick = now;
            if (!r->occupied) set_occupied(r, true);
        }
        if (r->occupied &&
            now - r->last_motion_tick > pdMS_TO_TICKS(ROOM_EMPTY_TIMEOUT_S * 1000))
            set_occupied(r, false);

        /* The heartbeat keeps the master informed even if a packet is lost */
        if (now - last_heartbeat > pdMS_TO_TICKS(HEARTBEAT_MS)) {
            last_heartbeat = now;
            send_status(r);
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

void room_start(int room_id, int pir_pin, int light_pin, int status_pin)
{
    if (room_id < 1 || room_id > 2) {
        ESP_LOGE(TAG, "invalid room id %d", room_id);
        return;
    }
    room_ctx_t *r = &room_ctx[room_id];
    memset(r, 0, sizeof(*r));
    r->id = room_id;
    r->pir_pin = pir_pin;
    r->light_pin = light_pin;
    r->status_pin = status_pin;

    /* GPIO34..39 are input-only and have no internal pull-down. */
    gpio_config_t in = {
        .pin_bit_mask = 1ULL << pir_pin,
        .mode         = GPIO_MODE_INPUT,
        .pull_down_en = (pir_pin < 34) ? GPIO_PULLDOWN_ENABLE : GPIO_PULLDOWN_DISABLE,
    };
    gpio_config(&in);

    uint64_t out_mask = 1ULL << light_pin;
    if (status_pin >= 0) out_mask |= 1ULL << status_pin;
    gpio_config_t out = { .pin_bit_mask = out_mask, .mode = GPIO_MODE_OUTPUT };
    gpio_config(&out);
    gpio_set_level(light_pin, 0);
    if (status_pin >= 0) gpio_set_level(status_pin, 0);

    char name[16];
    snprintf(name, sizeof(name), "room%d", room_id);
    xTaskCreate(room_task, name, 4096, r, 5, NULL);
    ESP_LOGI(TAG, "Room %d controller started (PIR GPIO%d, light GPIO%d)",
             room_id, pir_pin, light_pin);
}

#ifdef TERMINAL_ROOM
/* ---------- distributed build: ESP-NOW to the entrance ---------- */
void room_app_main(void)
{
    ESP_LOGI(TAG, "Room %d terminal starting", ROOM_ID);

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_now_init());

    esp_now_peer_info_t peer = { .channel = 0, .encrypt = false };
    uint8_t mac[] = ENTRANCE_MAC_ADDR;
    memcpy(peer.peer_addr, mac, 6);
    ESP_ERROR_CHECK(esp_now_add_peer(&peer));

    room_start(ROOM_ID, PIN_PIR, PIN_LIGHT, PIN_STATUS_LED);
    printf("Room %d ready - simulate motion via the PIR sensor in Wokwi\n", ROOM_ID);
}
#endif /* TERMINAL_ROOM */

#endif /* TERMINAL_ROOM || STANDALONE */

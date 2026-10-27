/*
 * main.c - common entry point. Dispatches to the entrance or the room
 * firmware depending on the -D TERMINAL_ENTRANCE / -D TERMINAL_ROOM flag.
 */
#include "esp_err.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "common.h"

#ifdef TERMINAL_ENTRANCE
void entrance_app_main(void);
#else
void room_app_main(void);
#endif

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

#ifdef TERMINAL_ENTRANCE
    entrance_app_main();
#else
    room_app_main();
#endif
}

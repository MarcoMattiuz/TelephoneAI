#include "app_entry.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "protocol_examples_common.h"

#include "openai_ws.h"
#include "time_sync.h"

static const char *TAG = "app_entry";

void app_run(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    ESP_ERROR_CHECK(example_connect());

    esp_err_t time_ret = update_time_from_nvs();
    if (time_ret != ESP_OK) {
        ESP_LOGW(TAG, "Could not restore time from NVS, syncing via SNTP");
        time_ret = fetch_and_store_time_in_nvs(NULL);
    }

    if (time_ret != ESP_OK) {
        ESP_LOGE(TAG, "System time synchronization failed, aborting websocket startup");
        return;
    }

    ESP_ERROR_CHECK(openai_ws_start());
}

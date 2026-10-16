#include "pcm_storage.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_spiffs.h"

static const char *TAG = "pcm_storage";

esp_err_t pcm_storage_init(void)
{
    esp_vfs_spiffs_conf_t conf = {
        .base_path = SPIFFS_BASE_PATH,
        .partition_label = "spiffs",
        .max_files = 4,
        .format_if_mount_failed = true,
    };

    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount SPIFFS: %s", esp_err_to_name(ret));
        return ret;
    }

    size_t total = 0, used = 0;
    esp_spiffs_info("spiffs", &total, &used);
    ESP_LOGI(TAG, "SPIFFS mounted: total=%u used=%u", total, used);

    // Remove stale capture file from previous run.
    remove(PCM_FILE_PATH);

    return ESP_OK;
}

esp_err_t pcm_storage_write(const int16_t *samples, size_t num_samples)
{
    if (samples == NULL || num_samples == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    FILE *f = fopen(PCM_FILE_PATH, "ab");
    if (f == NULL) {
        ESP_LOGE(TAG, "Failed to open %s for writing", PCM_FILE_PATH);
        return ESP_FAIL;
    }

    size_t written = fwrite(samples, sizeof(int16_t), num_samples, f);
    fclose(f);

    if (written != num_samples) {
        ESP_LOGE(TAG, "Partial write: %u of %u samples", written, num_samples);
        return ESP_FAIL;
    }

    return ESP_OK;
}

esp_err_t pcm_storage_get_size(size_t *size_out)
{
    if (size_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    FILE *f = fopen(PCM_FILE_PATH, "rb");
    if (f == NULL) {
        *size_out = 0;
        return ESP_ERR_NOT_FOUND;
    }

    fseek(f, 0, SEEK_END);
    *size_out = (size_t)ftell(f);
    fclose(f);

    return ESP_OK;
}

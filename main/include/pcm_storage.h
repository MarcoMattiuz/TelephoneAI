#pragma once

#include "esp_err.h"
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PCM_FILE_PATH "/spiffs/capture.pcm"
#define SPIFFS_BASE_PATH "/spiffs"

esp_err_t pcm_storage_init(void);
esp_err_t pcm_storage_write(const int16_t *samples, size_t num_samples);
esp_err_t pcm_storage_get_size(size_t *size_out);

#ifdef __cplusplus
}
#endif

#include "app_entry.h"
#include "esp_log.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdint.h>
#include <stdio.h>
#include "inmp441_i2s.h"

#define AUDIO_RING_BUFFER_SIZE (16 * 1024)
#define I2S_READ_CHUNK_BYTES 1024
#define AMP_CSV_MAX_LINE 4096
#define AMP_LOG_EVERY_N_CHUNKS 2

typedef struct {
    uint8_t data[AUDIO_RING_BUFFER_SIZE];
    size_t head;
    size_t tail;
    size_t count;
    portMUX_TYPE lock;
} audio_ring_buffer_t;

typedef struct {
    i2s_chan_handle_t rx_handle;
    audio_ring_buffer_t *ring;
    TickType_t read_timeout;
} i2s_capture_ctx_t;

static audio_ring_buffer_t s_audio_ring = {
    .head = 0,
    .tail = 0,
    .count = 0,
    .lock = portMUX_INITIALIZER_UNLOCKED,
};
static i2s_capture_ctx_t s_capture_ctx;
static char s_amp_line[AMP_CSV_MAX_LINE];
static uint32_t s_chunk_counter = 0;

static void audio_ring_buffer_write(audio_ring_buffer_t *ring, const uint8_t *src, size_t len)
{
    if (ring == NULL || src == NULL || len == 0) {
        return;
    }

    if (len > AUDIO_RING_BUFFER_SIZE) {
        src += (len - AUDIO_RING_BUFFER_SIZE);
        len = AUDIO_RING_BUFFER_SIZE;
    }

    portENTER_CRITICAL(&ring->lock);

    size_t free_space = AUDIO_RING_BUFFER_SIZE - ring->count;
    if (len > free_space) {
        size_t drop = len - free_space;
        ring->tail = (ring->tail + drop) % AUDIO_RING_BUFFER_SIZE;
        ring->count -= drop;
    }

    size_t first_part = AUDIO_RING_BUFFER_SIZE - ring->head;
    if (first_part > len) {
        first_part = len;
    }
    memcpy(&ring->data[ring->head], src, first_part);

    size_t second_part = len - first_part;
    if (second_part > 0) {
        memcpy(&ring->data[0], src + first_part, second_part);
    }

    ring->head = (ring->head + len) % AUDIO_RING_BUFFER_SIZE;
    ring->count += len;

    portEXIT_CRITICAL(&ring->lock);
}

static size_t audio_ring_buffer_available(const audio_ring_buffer_t *ring)
{
    if (ring == NULL) {
        return 0;
    }

    size_t count;
    portENTER_CRITICAL((portMUX_TYPE *)&ring->lock);
    count = ring->count;
    portEXIT_CRITICAL((portMUX_TYPE *)&ring->lock);

    return count;
}

// Decodes one 32-bit I2S slot into signed 24-bit PCM.
static int32_t decode_i2s_slot_to_s24(const uint8_t *slot)
{
    int32_t raw32 =
        ((int32_t)slot[0]) |
        ((int32_t)slot[1] << 8) |
        ((int32_t)slot[2] << 16) |
        ((int32_t)slot[3] << 24);

    return raw32 >> 8;
}

static void log_saved_frames_s24(const uint8_t *data, size_t len)
{
    if (data == NULL || len < sizeof(int32_t)) {
        return;
    }

    size_t frame_count = len / sizeof(int32_t);
    int written = snprintf(s_amp_line, sizeof(s_amp_line), "AMP,%lu,%u", (unsigned long)esp_log_timestamp(), (unsigned)frame_count);
    if (written < 0 || written >= (int)sizeof(s_amp_line)) {
        return;
    }

    size_t appended = 0;
    for (size_t i = 0; i < frame_count; ++i) {
        int32_t sample_s24 = decode_i2s_slot_to_s24(&data[i * sizeof(int32_t)]);
        int delta = snprintf(s_amp_line + written, sizeof(s_amp_line) - (size_t)written, ",%ld", (long)sample_s24);
        if (delta < 0 || delta >= (int)(sizeof(s_amp_line) - (size_t)written)) {
            break;
        }
        written += delta;
        appended++;
    }

    ESP_LOGI("main", "%s", s_amp_line);
    if (appended < frame_count) {
        ESP_LOGW("main", "AMP line truncated: appended=%u of %u frames", (unsigned)appended, (unsigned)frame_count);
    }
}

// Reads I2S continuously and stores bytes into a circular buffer.
static void i2s_stream_to_circular_buffer_task(void *arg)
{
    i2s_capture_ctx_t *ctx = (i2s_capture_ctx_t *)arg;
    uint8_t chunk[I2S_READ_CHUNK_BYTES];

    while (true) {
        size_t bytes_read = 0;
        esp_err_t ret = inmp441_i2s_read(
            ctx->rx_handle,
            chunk,
            sizeof(chunk),
            &bytes_read,
            ctx->read_timeout);

        if (ret == ESP_OK && bytes_read > 0) {
            audio_ring_buffer_write(ctx->ring, chunk, bytes_read);
            s_chunk_counter++;
            if ((s_chunk_counter % AMP_LOG_EVERY_N_CHUNKS) == 0) {
                log_saved_frames_s24(chunk, bytes_read);
            }
        } else if (ret != ESP_ERR_TIMEOUT) {
            ESP_LOGE("main", "I2S read error in capture task: %s", esp_err_to_name(ret));
        }
    }
}

static esp_err_t start_i2s_circular_capture(i2s_chan_handle_t rx_handle)
{
    s_capture_ctx.rx_handle = rx_handle;
    s_capture_ctx.ring = &s_audio_ring;
    s_capture_ctx.read_timeout = pdMS_TO_TICKS(200);

    BaseType_t task_ok = xTaskCreate(
        i2s_stream_to_circular_buffer_task,
        "i2s_capture",
        8192,
        &s_capture_ctx,
        5,
        NULL);

    if (task_ok != pdPASS) {
        ESP_LOGE("main", "Failed to start I2S capture task");
        return ESP_FAIL;
    }

    ESP_LOGI("main", "I2S circular capture started (buffer=%u bytes)", (unsigned)AUDIO_RING_BUFFER_SIZE);
    return ESP_OK;
}

void app_main(void)
{
    // app_run();

    inmp441_i2s_config_t i2s_config;
    inmp441_i2s_get_default_config(&i2s_config);
    i2s_chan_handle_t rx_handle;
    if (inmp441_i2s_init(&i2s_config, &rx_handle) != ESP_OK) {
        ESP_LOGE("main", "Failed to initialize I2S");
        return;
    }

    ESP_LOGI("main", "I2S initialized successfully");

    if (start_i2s_circular_capture(rx_handle) != ESP_OK) {
        return;
    }

    while (true)
    {
        ESP_LOGI("main", "Buffered audio bytes: %u", (unsigned)audio_ring_buffer_available(&s_audio_ring));
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    
}

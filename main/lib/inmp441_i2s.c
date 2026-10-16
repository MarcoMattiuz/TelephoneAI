#include "inmp441_i2s.h"

#include <string.h>

#include "esp_log.h"

static const char *TAG = "inmp441_i2s";
#define RING_BUFFER_SIZE 4096 // ogni slot sono 2 byte: 8KiB, ~256ms a 16kHz
#define TEST_ITERATIONS 10000
#define DUMP_EDGE_SAMPLES 64
#define DUMP_YIELD_EVERY 16
int16_t ring_buffer[RING_BUFFER_SIZE];
int32_t ring_buffer_head = 0;
int32_t ring_buffer_tail = 0;

void ring_buffer_write(int16_t sample) {
    ring_buffer[ring_buffer_head] = sample;
    ring_buffer_head = (ring_buffer_head + 1) % RING_BUFFER_SIZE;
    if (ring_buffer_head == ring_buffer_tail) {
        // Buffer overflow, move the tail forward to overwrite the oldest sample
        ring_buffer_tail = (ring_buffer_tail + 1) % RING_BUFFER_SIZE;
    }
}

void ring_buffer_read(int16_t *sample) {
    if (ring_buffer_head == ring_buffer_tail) {
        // Buffer is empty
        *sample = 0;
        return;
    }
    *sample = ring_buffer[ring_buffer_tail];
    ring_buffer_tail = (ring_buffer_tail + 1) % RING_BUFFER_SIZE;
}

void ring_buffer_dump(void) {
    int32_t total_samples;
    int32_t idx = ring_buffer_tail;
    int32_t printed = 0;

    if (ring_buffer_head >= ring_buffer_tail) {
        total_samples = ring_buffer_head - ring_buffer_tail;
    } else {
        total_samples = (RING_BUFFER_SIZE - ring_buffer_tail) + ring_buffer_head;
    }

    ESP_LOGI(TAG, "Ring buffer summary: samples=%ld, head=%ld, tail=%ld",
             (long)total_samples, (long)ring_buffer_head, (long)ring_buffer_tail);

    if (total_samples == 0) {
        ESP_LOGI(TAG, "Ring buffer is empty");
        return;
    }

    if (total_samples <= (2 * DUMP_EDGE_SAMPLES)) {
        while (idx != ring_buffer_head) {
            ESP_LOGI(TAG, "ring_buffer[%ld] = %d", (long)idx, (int)ring_buffer[idx]);
            idx = (idx + 1) % RING_BUFFER_SIZE;
            printed++;
            if ((printed % DUMP_YIELD_EVERY) == 0) {
                vTaskDelay(1);
            }
        }
        ESP_LOGI(TAG, "Ring buffer dump completed: printed=%ld", (long)printed);
        return;
    }

    ESP_LOGI(TAG, "Printing first %d and last %d samples", DUMP_EDGE_SAMPLES, DUMP_EDGE_SAMPLES);

    for (int32_t i = 0; i < DUMP_EDGE_SAMPLES; i++) {
        ESP_LOGI(TAG, "ring_buffer[%ld] = %d", (long)idx, (int)ring_buffer[idx]);
        idx = (idx + 1) % RING_BUFFER_SIZE;
        printed++;
        if ((printed % DUMP_YIELD_EVERY) == 0) {
            vTaskDelay(1);
        }
    }

    idx = (ring_buffer_head - DUMP_EDGE_SAMPLES + RING_BUFFER_SIZE) % RING_BUFFER_SIZE;
    for (int32_t i = 0; i < DUMP_EDGE_SAMPLES; i++) {
        ESP_LOGI(TAG, "ring_buffer[%ld] = %d", (long)idx, (int)ring_buffer[idx]);
        idx = (idx + 1) % RING_BUFFER_SIZE;
        printed++;
        if ((printed % DUMP_YIELD_EVERY) == 0) {
            vTaskDelay(1);
        }
    }

    ESP_LOGI(TAG, "Ring buffer dump completed: printed=%ld of %ld", (long)printed, (long)total_samples);
}

void inmp441_i2s_get_default_config(inmp441_i2s_config_t *config)
{
    if (config == NULL) {
        return;
    }

    *config = (inmp441_i2s_config_t) {
        .port = I2S_NUM_AUTO,
        .bclk_io_num = GPIO_NUM_32,
        .ws_io_num = GPIO_NUM_25,
        .data_in_io_num = GPIO_NUM_33,
        .sample_rate_hz = 16000,
        .slot_mask = I2S_STD_SLOT_LEFT,
        .dma_desc_num = 6,
        .dma_frame_num = 256,
    };
}

esp_err_t inmp441_i2s_init(const inmp441_i2s_config_t *config, i2s_chan_handle_t *rx_handle)
{
    if (config == NULL || rx_handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (config->bclk_io_num == GPIO_NUM_NC || config->ws_io_num == GPIO_NUM_NC || config->data_in_io_num == GPIO_NUM_NC) {
        ESP_LOGE(TAG, "I2S GPIOs are not configured");
        return ESP_ERR_INVALID_ARG;
    }

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(config->port, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = config->dma_desc_num;
    chan_cfg.dma_frame_num = config->dma_frame_num;

    esp_err_t ret = i2s_new_channel(&chan_cfg, NULL, rx_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2s_new_channel failed: %s", esp_err_to_name(ret));
        return ret;
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(config->sample_rate_hz),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = config->bclk_io_num,
            .ws = config->ws_io_num,
            .dout = I2S_GPIO_UNUSED,
            .din = config->data_in_io_num,
            .invert_flags = {
                .mclk_inv = 0,
                .bclk_inv = 0,
                .ws_inv = 0,
            },
        },
    };
    std_cfg.slot_cfg.slot_mask = config->slot_mask;

    ret = i2s_channel_init_std_mode(*rx_handle, &std_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_init_std_mode failed: %s", esp_err_to_name(ret));
        i2s_del_channel(*rx_handle);
        *rx_handle = NULL;
        return ret;
    }

    ret = i2s_channel_enable(*rx_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_enable failed: %s", esp_err_to_name(ret));
        i2s_del_channel(*rx_handle);
        *rx_handle = NULL;
        return ret;
    }

    return ESP_OK;
}

esp_err_t inmp441_i2s_deinit(i2s_chan_handle_t rx_handle)
{
    if (rx_handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = i2s_channel_disable(rx_handle);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        return ret;
    }

    return i2s_del_channel(rx_handle);
}


void inmp441_audio_capture_task(void *arg)
{
    (void)arg;

    inmp441_i2s_config_t i2s_config;
    inmp441_i2s_get_default_config(&i2s_config);

    i2s_chan_handle_t rx_handle = NULL;
    esp_err_t ret = inmp441_i2s_init(&i2s_config, &rx_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize INMP441 I2S: %s", esp_err_to_name(ret));
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "inmp441_audio_capture_task started");

    int32_t raw_sample;
    // Flush ring buffer state before new capture.
    ring_buffer_head = 0;
    ring_buffer_tail = 0;

    for (;;) {
        if (i2s_channel_read(rx_handle, &raw_sample, sizeof(raw_sample), NULL, portMAX_DELAY) == ESP_OK) {
            // INMP441: 24-bit data left-justified in 32-bit word → shift right 16 for signed PCM16
            int16_t pcm16 = (int16_t)(raw_sample >> 16);
            ring_buffer_write(pcm16);
        }
    }

    // Unreachable in current skeleton, kept for future stop path.
    (void)inmp441_i2s_deinit(rx_handle);
    vTaskDelete(NULL);
}

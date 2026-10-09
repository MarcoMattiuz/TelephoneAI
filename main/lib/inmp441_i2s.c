#include "inmp441_i2s.h"

#include <string.h>

#include "esp_log.h"

static const char *TAG = "inmp441_i2s";

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

esp_err_t inmp441_i2s_read(i2s_chan_handle_t rx_handle, void *data, size_t size, size_t *bytes_read, TickType_t ticks_to_wait)
{
    if (rx_handle == NULL || data == NULL || bytes_read == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    return i2s_channel_read(rx_handle, data, size, bytes_read, ticks_to_wait);
}

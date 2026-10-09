#pragma once

#include "driver/i2s_common.h"
#include "driver/i2s_std.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    i2s_port_t port;
    gpio_num_t bclk_io_num;
    gpio_num_t ws_io_num;
    gpio_num_t data_in_io_num;
    uint32_t sample_rate_hz;
    i2s_std_slot_mask_t slot_mask;
    uint32_t dma_desc_num;
    uint32_t dma_frame_num;
} inmp441_i2s_config_t;

void inmp441_i2s_get_default_config(inmp441_i2s_config_t *config);
esp_err_t inmp441_i2s_init(const inmp441_i2s_config_t *config, i2s_chan_handle_t *rx_handle);
esp_err_t inmp441_i2s_deinit(i2s_chan_handle_t rx_handle);
esp_err_t inmp441_i2s_read(i2s_chan_handle_t rx_handle, void *data, size_t size, size_t *bytes_read, TickType_t ticks_to_wait);
#ifdef __cplusplus
}
#endif

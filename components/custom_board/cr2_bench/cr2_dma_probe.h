#pragma once
#include "driver/i2s_std.h"
#include "esp_http_server.h"

void cr2_dma_probe_attach(i2s_chan_handle_t channel, uint32_t sample_rate,
                          uint32_t bits, uint32_t channels);
void cr2_dma_probe_start(void);
void cr2_dma_probe_stop(void);
void cr2_dma_probe_transfer_begin(size_t bytes, bool preload);
void cr2_dma_probe_transfer_end(size_t bytes, bool preload);
void cr2_dma_probe_snapshot(int queue_count);
uint32_t cr2_dma_probe_wait_budget(bool clock_error, int64_t *remaining_us);
void cr2_dma_probe_register_http(httpd_handle_t server);

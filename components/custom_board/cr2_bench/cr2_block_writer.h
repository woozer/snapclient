#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "esp_attr.h"
#include "cr2_dma_probe.h"

// Same 240-frame scratch size as the clean local-tone diagnostic.
// This variant copies the caller's bytes; no synthesis, gain or channel swap.
// The existing single player task owns both writes and preload.
static DRAM_ATTR int16_t cr2_block_pcm[240 * 2];
static esp_err_t cr2_block_transfer(i2s_chan_handle_t h, const void *data,
                                   size_t size, size_t *written,
                                   uint32_t timeout, bool preload) {
  *written = 0;
  if (size % 4) {
    cr2_audio_clock(false);
    return ESP_ERR_INVALID_SIZE;
  }
  const uint8_t *input = data;
  while (*written < size) {
    size_t count = size - *written;
    if (count > sizeof(cr2_block_pcm)) count = sizeof(cr2_block_pcm);
    memcpy(cr2_block_pcm, input + *written, count);
    size_t accepted = 0;
    cr2_dma_probe_transfer_begin(count, preload);
    esp_err_t result = preload
        ? i2s_channel_preload_data(h, cr2_block_pcm, count, &accepted)
        : i2s_channel_write(h, cr2_block_pcm, count, &accepted, timeout);
    cr2_dma_probe_transfer_end(accepted, preload);
    *written += accepted;
    if (result != ESP_OK) return result;
    // Preserve the diagnostic's and I2S API's partial-preload semantics.
    if (accepted < count) return ESP_OK;
  }
  return ESP_OK;
}

#pragma once
#include "cr2_inventory.h"
#include "driver/i2s_std.h"
#include "esp_attr.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

// Used only by player.c. No allocation, printing or PSRAM access in the ISR.
static portMUX_TYPE cr2_dma_mux = portMUX_INITIALIZER_UNLOCKED;
static uint64_t cr2_submitted, cr2_completed;
static int64_t cr2_eof_us;
static uint32_t cr2_byte_rate;
static bool cr2_dma_running;

static bool IRAM_ATTR cr2_dma_sent(i2s_chan_handle_t h,
                                  i2s_event_data_t *event, void *arg) {
    (void)h; (void)arg;
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL_ISR(&cr2_dma_mux);
    cr2_completed += event->size;
    cr2_eof_us = now;
    portEXIT_CRITICAL_ISR(&cr2_dma_mux);
    return false;
}
static void cr2_dma_stop(void) {
    portENTER_CRITICAL(&cr2_dma_mux);
    cr2_dma_running = false;
    cr2_submitted = cr2_completed = 0;
    cr2_eof_us = 0;
    portEXIT_CRITICAL(&cr2_dma_mux);
}
static void cr2_dma_start(void) {
    portENTER_CRITICAL(&cr2_dma_mux);
    cr2_completed = 0;
    cr2_eof_us = esp_timer_get_time();
    cr2_dma_running = true;
    portEXIT_CRITICAL(&cr2_dma_mux);
}
static void cr2_dma_written(size_t bytes) {
    portENTER_CRITICAL(&cr2_dma_mux);
    cr2_submitted += bytes;
    portEXIT_CRITICAL(&cr2_dma_mux);
}
static int64_t cr2_dma_remaining(void) {
    portENTER_CRITICAL(&cr2_dma_mux);
    uint64_t submitted=cr2_submitted, completed=cr2_completed;
    int64_t eof=cr2_eof_us;
    uint32_t rate=cr2_byte_rate;
    bool running=cr2_dma_running;
    portEXIT_CRITICAL(&cr2_dma_mux);
    return running ? cr2_inventory_us(submitted,completed,
                                     esp_timer_get_time()-eof,rate) : 0;
}

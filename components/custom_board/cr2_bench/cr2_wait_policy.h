#pragma once
#include "cr2_dma_core.h"
#include "cr2_dma_geometry.h"

typedef struct {
    bool enabled;
    uint32_t cap_us, reserve_us;
} cr2_wait_config_t;

static inline bool cr2_wait_config_valid(cr2_wait_config_t c) {
    return c.cap_us>=1000 && c.cap_us<=40000 && c.reserve_us>=3000 &&
           c.reserve_us<=15000 && c.cap_us%1000==0 && c.reserve_us%1000==0;
}

/* Trial default: keep five milliseconds for dispatch and the next I2S write.
 * Timeout is rounded DOWN to RTOS ticks by the caller. Never add playout delay.
 * Unknown accounting, unusually delayed EOF, or clock-error resync gets no wait.
 */
static inline uint32_t cr2_wait_budget(cr2_dma_inventory_t inventory,
                                     uint32_t max_eof_gap_us,
                                     bool clock_error, cr2_wait_config_t config,
                                     uint32_t descriptor_us, uint32_t tick_us) {
    if (!config.enabled || !cr2_wait_config_valid(config) ||
        clock_error || inventory.flags || inventory.estimate_us<=config.reserve_us ||
        !descriptor_us || !tick_us ||
        inventory.since_eof_us<0 || inventory.since_eof_us>descriptor_us ||
        max_eof_gap_us>descriptor_us+tick_us) return 0;
    int64_t available=inventory.estimate_us-config.reserve_us;
    return available>config.cap_us ? config.cap_us : (uint32_t)available;
}
static inline bool cr2_wait_accept(uint32_t initial_budget_us,
                                   int64_t elapsed_us, bool queued,
                                   uint32_t remaining_budget_us) {
    return queued && elapsed_us>=0 && elapsed_us<=initial_budget_us &&
           remaining_budget_us>0;
}

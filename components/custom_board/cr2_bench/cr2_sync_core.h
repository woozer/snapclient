#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

// Diagnostic state only. Never modifies PCM, I2S clocks, volume or power state.
typedef struct {
    uint32_t until_ms, requested_insert, requested_drop, bypassed;
    uint32_t selected_insert, selected_drop, writes, frames, malformed;
    int32_t last_age_us, last_short_us, last_mini_us;
    uint32_t max_boundary_delta[2];
    int16_t previous[2], max_before[2], max_after[2];
    bool have_previous;
} cr2_sync_state_t;

static inline bool cr2_sync_active(const cr2_sync_state_t *s, uint32_t now) {
    return s->until_ms && (int32_t)(s->until_ms - now) > 0;
}

static inline void cr2_sync_reset(cr2_sync_state_t *s, uint32_t now, bool bypass) {
    memset(s, 0, sizeof(*s));
    if (bypass) {
        s->until_ms = now + 45000u;
        if (!s->until_ms) s->until_ms = 1;
    }
}

// -1 repeats a frame, +1 omits a frame. Only the optional 45s experiment
// overrides the existing control loop's decision. Hard resync stays upstream.
static inline int cr2_sync_choose(cr2_sync_state_t *s, uint32_t now,
                                 int requested, int64_t age,
                                 int64_t short_us, int64_t mini_us) {
    if (requested != -1 && requested != 1) return requested;
    s->last_age_us = (int32_t)age;
    s->last_short_us = (int32_t)short_us;
    s->last_mini_us = (int32_t)mini_us;
    if (requested < 0) ++s->requested_insert;
    else ++s->requested_drop;
    if (cr2_sync_active(s, now)) {
        ++s->bypassed;
        return 0;
    }
    if (requested < 0) ++s->selected_insert;
    else ++s->selected_drop;
    return requested;
}

static inline int16_t cr2_sync_s16le(const uint8_t *p) {
    uint16_t value = (uint16_t)p[0] | ((uint16_t)p[1] << 8);
    return value < 32768u ? (int16_t)value : (int16_t)((int32_t)value - 65536);
}

// Observe accepted writes, not physical LRCLK/DMA output. Compare only write
// boundaries; this does not inspect every sample or establish an audible click.
static inline void cr2_sync_observe(cr2_sync_state_t *s,
                                   const void *data, size_t size) {
    if (!size) return;
    if (!data || size % 4) { ++s->malformed; return; }
    const uint8_t *p = data;
    for (unsigned channel = 0; channel < 2; ++channel) {
        int16_t first = cr2_sync_s16le(p + 2 * channel);
        if (s->have_previous) {
            int delta = (int)first - (int)s->previous[channel];
            uint32_t magnitude = delta < 0 ? (uint32_t)-delta : (uint32_t)delta;
            if (magnitude > s->max_boundary_delta[channel]) {
                s->max_boundary_delta[channel] = magnitude;
                s->max_before[channel] = s->previous[channel];
                s->max_after[channel] = first;
            }
        }
        s->previous[channel] = cr2_sync_s16le(p + size - 4 + 2 * channel);
    }
    s->have_previous = true;
    ++s->writes;
    s->frames += size / 4;
}

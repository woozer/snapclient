#pragma once
#include <stdbool.h>
#include <stdint.h>

/* Diagnostic only. EOF reports a descriptor entering the peripheral, not a
 * measured speaker sample. Subtract a whole descriptor for the coarse bound.
 * Invalidate rather than infer inventory after a missed/reused descriptor.
 */
enum {
    CR2_DMA_NO_PREFILL=1, CR2_DMA_Q_OVERFLOW=2, CR2_DMA_CALLBACK=4,
    CR2_DMA_EOF_GAP=8, CR2_DMA_BALANCE=16, CR2_DMA_STOPPED=32,
    CR2_DMA_PARTIAL_ACTIVE=64
};
typedef struct {
    uint64_t submitted, completed;
    int64_t start_us, eof_us;
    uint32_t rate, block, capacity, flags, eof_count, overflows, max_gap_us;
    uint32_t inflight;
    bool running;
} cr2_dma_state_t;
typedef struct {
    int64_t balance, lower_us, estimate_us, since_eof_us;
    uint32_t flags;
} cr2_dma_inventory_t;

static inline cr2_dma_inventory_t cr2_dma_inventory(const cr2_dma_state_t *s,
                                                   int64_t now) {
    cr2_dma_inventory_t o = {.balance=(int64_t)s->submitted-(int64_t)s->completed,
                            .flags=s->flags};
    o.since_eof_us=now-(s->eof_us ? s->eof_us : s->start_us);
    if (!s->running) o.flags|=CR2_DMA_STOPPED;
    if (!s->rate || !s->block || o.balance<0 || o.balance>s->capacity ||
        o.since_eof_us<0) o.flags|=CR2_DMA_BALANCE;
    if (s->rate && o.since_eof_us>(int64_t)s->block*1500000/s->rate)
        o.flags|=CR2_DMA_EOF_GAP;
    o.lower_us=o.estimate_us=-1;
    if (!o.flags) {
        o.lower_us=o.balance>s->block ? (o.balance-s->block)*1000000/s->rate : 0;
        o.estimate_us=o.balance*1000000/s->rate-o.since_eof_us;
        if (o.estimate_us<0) o.estimate_us=0;
    }
    return o;
}

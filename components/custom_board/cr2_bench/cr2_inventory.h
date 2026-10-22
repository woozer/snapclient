#pragma once
#include <stdint.h>

// EOF counts complete DMA descriptors; elapsed time covers the descriptor
// currently being transmitted. Negative inventory means the deadline passed.
static inline int64_t cr2_inventory_us(uint64_t submitted, uint64_t completed,
                                      int64_t since_eof_us, uint32_t byte_rate) {
    if (!byte_rate) return 0;
    int64_t bytes = (int64_t)submitted - (int64_t)completed;
    return bytes * 1000000LL / byte_rate - since_eof_us;
}

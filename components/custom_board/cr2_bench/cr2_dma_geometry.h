#pragma once
#include <stdint.h>
#ifndef CR2_DMA_FRAMES
#define CR2_DMA_FRAMES 480
#endif
#define CR2_DMA_DESCRIPTORS 2
static inline uint32_t cr2_dma_period_us(uint32_t bytes,uint32_t byte_rate) {
    return byte_rate ? ((uint64_t)bytes*1000000+byte_rate-1)/byte_rate : 0;
}

#pragma once
#include <stdbool.h>
#include <stdint.h>

/* Prepare before the original PTS deadline. The two-millisecond allowance
 * is dispatch/API jitter after the alarm, never permission to prefill late. */
#define CR2_STARTUP_ARM_GUARD_US 1000
#define CR2_STARTUP_DISPATCH_LIMIT_US 2000
static inline uint32_t cr2_startup_queue_ticks(int64_t deadline, int64_t now,
                                              uint32_t tick_hz) {
    int64_t left=deadline-now-CR2_STARTUP_ARM_GUARD_US;
    if(left<=0 || !tick_hz) return 0;
    uint64_t ticks=(uint64_t)left*tick_hz/1000000;
    return ticks>UINT32_MAX ? UINT32_MAX : (uint32_t)ticks;
}
static inline bool cr2_startup_can_arm(int64_t deadline,int64_t now) {
    return deadline-now>CR2_STARTUP_ARM_GUARD_US;
}
static inline bool cr2_startup_can_enable(int64_t deadline,int64_t now) {
    return now>=deadline && now-deadline<=CR2_STARTUP_DISPATCH_LIMIT_US;
}
static inline int64_t cr2_startup_cursor_pts(int64_t first,uint64_t frames,
                                             uint32_t sample_rate) {
    return first+(int64_t)(frames*1000000/sample_rate);
}

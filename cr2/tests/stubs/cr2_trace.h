#pragma once
#include <stdint.h>
static unsigned trace_count;
static inline void cr2_trace(int k,int64_t a,int64_t b,int64_t c,int d){(void)k;(void)a;(void)b;(void)c;(void)d;trace_count++;}

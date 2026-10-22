#pragma once
#include <stdint.h>

// RAM-only diagnostic events, frozen before HTTP export; no per-packet printing.
void cr2_trace_init(void);
void cr2_trace(int kind, int64_t a, int64_t b, int64_t c, int d);
void cr2_trace_register_http(void *server);

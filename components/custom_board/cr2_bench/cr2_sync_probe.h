#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_http_server.h"

int cr2_sync_decision(int requested, int64_t age, int64_t short_us, int64_t mini_us);
void cr2_sync_written(const void *data, size_t size);
void cr2_sync_discontinuity(void);
void cr2_sync_register_http(httpd_handle_t server);
// Implemented alongside the existing board-control state; read-only predicate.
bool cr2_sync_can_configure(void);

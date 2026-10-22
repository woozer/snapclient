#pragma once
#include <stdbool.h>
#include "esp_http_server.h"
bool cr2_audio_begin_update(void);
void cr2_audio_end_update(void);
void cr2_ota_register_http(httpd_handle_t server);
const char *cr2_ota_running_slot(void);

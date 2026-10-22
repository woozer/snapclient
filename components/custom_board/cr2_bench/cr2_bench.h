#pragma once
#include "audio_hal.h"
#include "esp_http_server.h"

// Playback bench: explicit timed arm, bounded gain, charge always LOW.
void cr2_bench_early_init(void);
void cr2_audio_clock(bool running);
void cr2_audio_written(void);
void cr2_audio_format(int rate, int bits, int channels);
void cr2_audio_resync(int kind);
void cr2_audio_buffer(bool recovered);
void cr2_audio_register_http(httpd_handle_t server);
// Commands are applied by the network task, which owns snapcastSetting_t.
bool cr2_audio_take_control(int *volume, bool *muted);
void cr2_audio_server_settings(int *volume, bool *muted);
extern audio_hal_func_t AUDIO_CODEC_CR2_BENCH_DEFAULT_HANDLE;

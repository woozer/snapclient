#pragma once
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "cr2_dma_geometry.h"
typedef int esp_err_t;
typedef void *i2s_chan_handle_t;
typedef struct {size_t total_dma_buf_size;} i2s_chan_info_t;
typedef struct {size_t size; void *dma_buf;} i2s_event_data_t;
typedef bool (*cb_t)(i2s_chan_handle_t,i2s_event_data_t*,void*);
typedef struct {cb_t on_sent,on_send_q_ovf;} i2s_event_callbacks_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERROR_CHECK(x) assert((x)==ESP_OK)
static inline esp_err_t i2s_channel_get_info(void *c,i2s_chan_info_t *i){(void)c;i->total_dma_buf_size=CR2_DMA_FRAMES*4*CR2_DMA_DESCRIPTORS;return 0;}
static inline esp_err_t i2s_channel_register_event_callback(void *c,const i2s_event_callbacks_t *b,void *u){(void)c;(void)b;(void)u;return 0;}

#include "cr2_dma_probe.h"
#include "cr2_dma_core.h"
#include "cr2_wait_policy.h"
#include "cr2_sync_probe.h"
#include "cJSON.h"
#include "cr2_trace.h"
#include "esp_attr.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

static DRAM_ATTR cr2_dma_state_t state;
static DRAM_ATTR portMUX_TYPE mux=portMUX_INITIALIZER_UNLOCKED;
static DRAM_ATTR void *previous_buffer;
static unsigned epochs, snapshots;
static cr2_dma_inventory_t last;
static cr2_wait_config_t tuning={.enabled=true,.cap_us=20000,.reserve_us=5000};

/* Internal RAM only; no allocation, logging, trace/PSRAM, or playback decision
 * in ISR. IDF clears/queues the descriptor after this callback returns.
 */
static bool IRAM_ATTR sent(i2s_chan_handle_t channel, i2s_event_data_t *event,
                          void *context) {
    (void)channel; (void)context;
    int64_t now=esp_timer_get_time();
    portENTER_CRITICAL_ISR(&mux);
    if (state.running) {
        int64_t gap=now-(state.eof_us ? state.eof_us : state.start_us);
        if (gap>state.max_gap_us) state.max_gap_us=(uint32_t)gap;
        if (event->size!=state.block || event->dma_buf==previous_buffer)
            state.flags|=CR2_DMA_CALLBACK;
        /* 48k stereo S16, 1023-frame descriptors: nominal 21312.5 us.
         * No division/helper from the ISR; attach rejects other formats. */
        if (gap>32000) state.flags|=CR2_DMA_EOF_GAP;
        state.completed+=event->size;
        state.eof_count++;
        state.eof_us=now;
        previous_buffer=event->dma_buf;
        /* If the following descriptor cannot be fully populated even allowing
         * the small in-flight write, continuity is unknown for this epoch. */
        if (state.submitted+state.inflight<state.completed+state.block)
            state.flags|=CR2_DMA_PARTIAL_ACTIVE;
    }
    portEXIT_CRITICAL_ISR(&mux);
    return false;
}
static bool IRAM_ATTR overflow(i2s_chan_handle_t channel, i2s_event_data_t *event,
                              void *context) {
    (void)channel; (void)event; (void)context;
    portENTER_CRITICAL_ISR(&mux);
    if (state.running) {
        state.overflows++;
        state.flags|=CR2_DMA_Q_OVERFLOW;
    }
    portEXIT_CRITICAL_ISR(&mux);
    return false;
}
void cr2_dma_probe_attach(i2s_chan_handle_t channel, uint32_t sr,
                          uint32_t bits, uint32_t channels) {
    i2s_chan_info_t info;
    ESP_ERROR_CHECK(i2s_channel_get_info(channel, &info));
    portENTER_CRITICAL(&mux);
    memset(&state,0,sizeof(state));
    state.rate=sr*channels*(bits/8);
    state.capacity=info.total_dma_buf_size;
    state.block=4092;
    state.flags=(sr==48000 && bits==16 && channels==2 &&
                 info.total_dma_buf_size==8184) ? 0 : CR2_DMA_CALLBACK;
    portEXIT_CRITICAL(&mux);
    i2s_event_callbacks_t callbacks={.on_sent=sent, .on_send_q_ovf=overflow};
    ESP_ERROR_CHECK(i2s_channel_register_event_callback(channel,&callbacks,NULL));
}
void cr2_dma_probe_stop(void) {
    portENTER_CRITICAL(&mux);
    state.running=false;
    state.submitted=state.completed=0;
    state.start_us=state.eof_us=0;
    state.eof_count=state.overflows=state.max_gap_us=state.inflight=0;
    state.flags=(state.rate==192000 && state.capacity==8184) ? 0 : CR2_DMA_CALLBACK;
    previous_buffer=NULL;
    portEXIT_CRITICAL(&mux);
}
void cr2_dma_probe_start(void) {
    portENTER_CRITICAL(&mux);
    epochs++;
    if (state.submitted!=state.capacity) state.flags|=CR2_DMA_NO_PREFILL;
    state.running=true;
    state.start_us=esp_timer_get_time();
    portEXIT_CRITICAL(&mux);
}
void cr2_dma_probe_transfer_begin(size_t bytes, bool preload) {
    (void)preload;
    portENTER_CRITICAL(&mux);
    state.inflight=bytes;
    portEXIT_CRITICAL(&mux);
}
void cr2_dma_probe_transfer_end(size_t bytes, bool preload) {
    (void)preload;
    portENTER_CRITICAL(&mux);
    state.submitted+=bytes;
    state.inflight=0;
    if (state.running && state.submitted<state.completed)
        state.flags|=CR2_DMA_BALANCE;
    portEXIT_CRITICAL(&mux);
}
uint32_t cr2_dma_probe_wait_budget(bool clock_error, int64_t *remaining_us) {
    portENTER_CRITICAL(&mux);
    int64_t now=esp_timer_get_time();
    cr2_dma_state_t s=state;
    cr2_wait_config_t config=tuning;
    portEXIT_CRITICAL(&mux);
    cr2_dma_inventory_t o=cr2_dma_inventory(&s,now);
    *remaining_us=o.estimate_us;
    return cr2_wait_budget(o,s.max_gap_us,clock_error,config);
}
void cr2_dma_probe_snapshot(int queue_count) {
    portENTER_CRITICAL(&mux);
    int64_t now=esp_timer_get_time();
    cr2_dma_state_t s=state;
    portEXIT_CRITICAL(&mux);
    cr2_dma_inventory_t o=cr2_dma_inventory(&s,now);
    portENTER_CRITICAL(&mux);
    snapshots++; last=o;
    unsigned epoch=epochs;
    portEXIT_CRITICAL(&mux);
    /* Four task-context records, all refer to the same atomic snapshot time. */
    cr2_trace(11,o.lower_us,o.estimate_us,o.since_eof_us,o.flags);
    cr2_trace(12,s.submitted,s.completed,now,queue_count);
    cr2_trace(13,s.eof_count,s.overflows,s.max_gap_us,epoch);
    cr2_trace(14,s.rate,s.block,s.capacity,s.inflight);
}
static esp_err_t status(httpd_req_t *req) {
    portENTER_CRITICAL(&mux);
    cr2_dma_state_t s=state;
    cr2_dma_inventory_t l=last;
    cr2_wait_config_t config=tuning;
    unsigned n=snapshots,e=epochs;
    portEXIT_CRITICAL(&mux);
    char out[768];
    int count=snprintf(out,sizeof(out),
        "{\"descriptor_probe_enabled\":true,\"physical_lrclk_measured\":false,"
        "\"playback_logic_changed\":true,\"bounded_wait_enabled\":%s,"
        "\"wait_cap_us\":%u,\"reserve_us\":%u,\"epochs\":%u,\"snapshots\":%u,"
        "\"running\":%s,\"submitted\":%"PRIu64",\"completed\":%"PRIu64","
        "\"eof_count\":%u,\"overflows\":%u,\"flags\":%u,"
        "\"last_lower_us\":%"PRId64",\"last_estimate_us\":%"PRId64","
        "\"last_flags\":%u,\"scope\":\"EOF_and_accepted_bytes; invalid_epochs_unknown\"}",
        config.enabled?"true":"false",(unsigned)config.cap_us,(unsigned)config.reserve_us,
        e,n,s.running?"true":"false",s.submitted,s.completed,
        (unsigned)s.eof_count,(unsigned)s.overflows,(unsigned)s.flags,
        l.lower_us,l.estimate_us,(unsigned)l.flags);
    if (count<0 || (size_t)count>=sizeof(out)) return ESP_FAIL;
    httpd_resp_set_type(req,"application/json");
    httpd_resp_set_hdr(req,"Cache-Control","no-store");
    return httpd_resp_sendstr(req,out);
}
static esp_err_t tuning_status(httpd_req_t *req) {
    portENTER_CRITICAL(&mux);
    cr2_wait_config_t c=tuning;
    portEXIT_CRITICAL(&mux);
    char out[640];
    int n=snprintf(out,sizeof(out),
        "{\"enabled\":%s,\"max_wait_ms\":%u,\"reserve_ms\":%u,"
        "\"persistent\":false,\"requires_disarmed\":true,"
        "\"limits\":{\"max_wait_ms\":[1,40],\"reserve_ms\":[3,15]},"
        "\"read_only\":{\"dma_frames\":1023,\"dma_descriptors\":2,"
        "\"dma_geometry_mutable\":false,\"snapcast_buffer_owner\":\"server\","
        "\"tcp_window_bytes\":32768,\"tcp_mailbox\":32}}",
        c.enabled?"true":"false",(unsigned)c.cap_us/1000,(unsigned)c.reserve_us/1000);
    if (n<0 || (size_t)n>=sizeof(out)) return ESP_FAIL;
    httpd_resp_set_type(req,"application/json");
    httpd_resp_set_hdr(req,"Cache-Control","no-store");
    return httpd_resp_sendstr(req,out);
}
static esp_err_t tuning_set(httpd_req_t *req) {
    char header[8],type[32],body[161];
    if (httpd_req_get_hdr_value_str(req,"X-CR2-Control",header,sizeof(header))!=ESP_OK ||
        strcmp(header,"1") ||
        httpd_req_get_hdr_value_str(req,"Content-Type",type,sizeof(type))!=ESP_OK ||
        strcmp(type,"application/json") || req->content_len<1 ||
        req->content_len>=sizeof(body))
        return httpd_resp_send_err(req,HTTPD_400_BAD_REQUEST,"Expected bounded JSON tuning");
    size_t read=0;
    while (read<(size_t)req->content_len) {
        int n=httpd_req_recv(req,body+read,req->content_len-read);
        if(n<=0) return httpd_resp_send_err(req,HTTPD_400_BAD_REQUEST,"Incomplete JSON");
        read+=n;
    }
    body[read]=0;
    if(strlen(body)!=read) return httpd_resp_send_err(req,HTTPD_400_BAD_REQUEST,"Embedded NUL");
    const char *end=NULL;
    cJSON *root=cJSON_ParseWithOpts(body,&end,true);
    cJSON *enabled=cJSON_GetObjectItemCaseSensitive(root,"enabled");
    cJSON *cap=cJSON_GetObjectItemCaseSensitive(root,"max_wait_ms");
    cJSON *reserve=cJSON_GetObjectItemCaseSensitive(root,"reserve_ms");
    bool valid=cJSON_IsObject(root) && cJSON_GetArraySize(root)==3 &&
        cJSON_IsBool(enabled) && cJSON_IsNumber(cap) && cJSON_IsNumber(reserve) &&
        cap->valuedouble>=1 && cap->valuedouble<=40 &&
        reserve->valuedouble>=3 && reserve->valuedouble<=15 &&
        cap->valuedouble==cap->valueint && reserve->valuedouble==reserve->valueint;
    cr2_wait_config_t c={0};
    if(valid) {
        c=(cr2_wait_config_t){.enabled=cJSON_IsTrue(enabled),
            .cap_us=(uint32_t)cap->valueint*1000,.reserve_us=(uint32_t)reserve->valueint*1000};
        valid=cr2_wait_config_valid(c);
    }
    cJSON_Delete(root);
    if(!valid)
        return httpd_resp_send_err(req,HTTPD_400_BAD_REQUEST,
            "Use enabled:boolean, max_wait_ms:1..40, reserve_ms:3..15; integers only");
    if(!cr2_sync_can_configure())
        return httpd_resp_send_err(req,HTTPD_400_BAD_REQUEST,"Disarm playback first; no charging or OTA");
    portENTER_CRITICAL(&mux);
    tuning=c;
    portEXIT_CRITICAL(&mux);
    return tuning_status(req);
}
void cr2_dma_probe_register_http(httpd_handle_t server) {
    const httpd_uri_t uri={.uri="/cr2/dma",.method=HTTP_GET,.handler=status};
    ESP_ERROR_CHECK(httpd_register_uri_handler(server,&uri));
    const httpd_uri_t get={.uri="/cr2/tuning",.method=HTTP_GET,.handler=tuning_status};
    const httpd_uri_t post={.uri="/cr2/tuning",.method=HTTP_POST,.handler=tuning_set};
    ESP_ERROR_CHECK(httpd_register_uri_handler(server,&get));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server,&post));
}

#include "cr2_trace.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

#define TRACE_CAPACITY 98304
#define TRACE_WINDOW_US (150LL * 1000000)
typedef struct {
    int64_t at, a, b, c;
    int32_t kind, d;
} trace_event_t;
static trace_event_t *events;
static unsigned count, dropped;
static bool frozen;
static int64_t started, stopped;
static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;

void cr2_trace_init(void) {
    events = heap_caps_calloc(TRACE_CAPACITY, sizeof(*events), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!events) abort();
    started = esp_timer_get_time();
}

void cr2_trace(int kind, int64_t a, int64_t b, int64_t c, int d) {
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&mux);
    if (!frozen && events) {
        if (now - started >= TRACE_WINDOW_US) {
            frozen = true;
            stopped = now;
        } else if (count == TRACE_CAPACITY) {
            dropped++;
        } else {
            events[count++] = (trace_event_t){now, a, b, c, kind, d};
        }
    }
    portEXIT_CRITICAL(&mux);
}

static esp_err_t trace_get(httpd_req_t *req) {
    portENTER_CRITICAL(&mux);
    if (!frozen) { frozen = true; stopped = esp_timer_get_time(); }
    unsigned n = count, lost = dropped;
    portEXIT_CRITICAL(&mux);
    httpd_resp_set_type(req, "text/csv");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    char buf[2048];
    int used = snprintf(buf, sizeof(buf),
                        "# count=%u dropped=%u started_us=%" PRId64 " stopped_us=%" PRId64 "\n"
                        "kind,at_us,a,b,c,d\n", n, lost, started, stopped);
    if (used < 0 || used >= sizeof(buf)) return ESP_FAIL;
    for (unsigned i = 0; i < n; i++) {
        if (sizeof(buf) - used < 160) {
            esp_err_t err = httpd_resp_send_chunk(req, buf, used);
            if (err != ESP_OK) return err;
            used = 0;
        }
        const trace_event_t *e = &events[i];
        int len = snprintf(buf + used, sizeof(buf) - used,
                           "%d,%" PRId64 ",%" PRId64 ",%" PRId64 ",%" PRId64 ",%d\n",
                           (int)e->kind, e->at, e->a, e->b, e->c, (int)e->d);
        if (len < 0 || len >= sizeof(buf) - used) return ESP_FAIL;
        used += len;
    }
    if (used) {
        esp_err_t err = httpd_resp_send_chunk(req, buf, used);
        if (err != ESP_OK) return err;
    }
    return httpd_resp_send_chunk(req, NULL, 0);
}

void cr2_trace_register_http(void *server) {
    const httpd_uri_t uri = {.uri = "/bench-trace", .method = HTTP_GET, .handler = trace_get};
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &uri));
}

#include "cr2_sync_probe.h"
#include "cr2_sync_core.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include <stdio.h>
#include <string.h>

static cr2_sync_state_t state;
static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;

static uint32_t milliseconds(void) {
    return (uint32_t)(esp_timer_get_time() / 1000);
}

int cr2_sync_decision(int requested, int64_t age, int64_t short_us, int64_t mini_us) {
    uint32_t now = milliseconds();
    portENTER_CRITICAL(&mux);
    int selected = cr2_sync_choose(&state, now, requested, age, short_us, mini_us);
    portEXIT_CRITICAL(&mux);
    return selected;
}

void cr2_sync_written(const void *data, size_t size) {
    portENTER_CRITICAL(&mux);
    cr2_sync_observe(&state, data, size);
    portEXIT_CRITICAL(&mux);
}

void cr2_sync_discontinuity(void) {
    portENTER_CRITICAL(&mux);
    state.have_previous = false; // Do not count restart/preload gaps as PCM edges.
    portEXIT_CRITICAL(&mux);
}

static esp_err_t status(httpd_req_t *req) {
    cr2_sync_state_t s;
    uint32_t now = milliseconds();
    portENTER_CRITICAL(&mux);
    s = state;
    portEXIT_CRITICAL(&mux);
    char out[1024];
    bool active = cr2_sync_active(&s, now);
    int n = snprintf(out, sizeof(out),
        "{\"mode\":\"%s\",\"remaining_ms\":%u,"
        "\"requested_insert\":%u,\"requested_drop\":%u,\"bypassed\":%u,"
        "\"selected_insert\":%u,\"selected_drop\":%u,\"writes\":%u,\"frames\":%u,"
        "\"malformed\":%u,\"last_age_us\":%ld,\"last_short_us\":%ld,\"last_mini_us\":%ld,"
        "\"max_boundary_delta\":[%u,%u],\"max_before\":[%d,%d],\"max_after\":[%d,%d],"
        "\"boundary_scope\":\"accepted_I2S_write_boundaries_not_physical_output\","
        "\"hard_resync_retained\":true,\"dma_monitor_enabled\":false}",
        active ? "bypass" : "observe", (unsigned)(active ? s.until_ms - now : 0u),
        (unsigned)s.requested_insert, (unsigned)s.requested_drop, (unsigned)s.bypassed,
        (unsigned)s.selected_insert, (unsigned)s.selected_drop, (unsigned)s.writes,
        (unsigned)s.frames, (unsigned)s.malformed, (long)s.last_age_us,
        (long)s.last_short_us, (long)s.last_mini_us,
        (unsigned)s.max_boundary_delta[0], (unsigned)s.max_boundary_delta[1],
        s.max_before[0], s.max_before[1], s.max_after[0], s.max_after[1]);
    if (n < 0 || n >= sizeof(out)) return ESP_FAIL;
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, out);
}

static esp_err_t configure(httpd_req_t *req) {
    char header[8], type[24], body[9];
    if (httpd_req_get_hdr_value_str(req, "X-CR2-Control", header, sizeof(header)) != ESP_OK ||
        strcmp(header, "1") ||
        httpd_req_get_hdr_value_str(req, "Content-Type", type, sizeof(type)) != ESP_OK ||
        strcmp(type, "text/plain") || req->content_len < 1 || req->content_len >= sizeof(body))
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Expected bounded probe command");
    size_t count = 0;
    while (count < req->content_len) {
        int n = httpd_req_recv(req, body + count, req->content_len - count);
        if (n <= 0) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Incomplete command");
        count += n;
    }
    body[count] = 0;
    bool bypass = count == 6 && !memcmp(body, "bypass", 6);
    bool observe = count == 7 && !memcmp(body, "observe", 7);
    if ((!bypass && !observe) || !cr2_sync_can_configure())
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Disarm first; use observe or bypass");
    uint32_t now = milliseconds();
    portENTER_CRITICAL(&mux);
    cr2_sync_reset(&state, now, bypass);
    portEXIT_CRITICAL(&mux);
    return status(req);
}

void cr2_sync_register_http(httpd_handle_t server) {
    const httpd_uri_t get = {.uri="/cr2/sync", .method=HTTP_GET, .handler=status};
    const httpd_uri_t post = {.uri="/cr2/sync", .method=HTTP_POST, .handler=configure};
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &get));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &post));
}

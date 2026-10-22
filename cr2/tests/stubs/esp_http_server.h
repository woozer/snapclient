#pragma once
#include "driver/i2s_std.h"
typedef void *httpd_handle_t;
typedef struct {size_t content_len;} httpd_req_t;
typedef struct {const char *uri;int method;esp_err_t (*handler)(httpd_req_t*);} httpd_uri_t;
#define HTTP_GET 0
static inline int httpd_register_uri_handler(void *s,const httpd_uri_t *u){(void)s;(void)u;return 0;}
static inline int httpd_resp_set_type(void *r,const char *s){(void)r;(void)s;return 0;}
static inline int httpd_resp_set_hdr(void *r,const char *s,const char *v){(void)r;(void)s;(void)v;return 0;}
static inline int httpd_resp_sendstr(void *r,const char *s){(void)r;(void)s;return 0;}

#define HTTP_POST 1
#define HTTPD_400_BAD_REQUEST 400
static inline int httpd_req_get_hdr_value_str(void *r,const char *n,char *o,size_t s){(void)r;(void)n;(void)o;(void)s;return -1;}
static inline int httpd_req_recv(void *r,char *b,size_t n){(void)r;(void)b;(void)n;return -1;}
static inline int httpd_resp_send_err(void *r,int c,const char *s){(void)r;(void)c;(void)s;return -1;}

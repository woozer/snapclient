#pragma once
#include <stdbool.h>
#include <stddef.h>
typedef struct {double valuedouble; int valueint;} cJSON;
static inline cJSON *cJSON_ParseWithOpts(const char *s,const char **e,bool n){(void)s;(void)e;(void)n;return NULL;}
static inline cJSON *cJSON_GetObjectItemCaseSensitive(const cJSON *r,const char *s){(void)r;(void)s;return NULL;}
static inline int cJSON_GetArraySize(const cJSON *r){(void)r;return 0;}
static inline bool cJSON_IsObject(const cJSON *r){(void)r;return false;}
static inline bool cJSON_IsBool(const cJSON *r){(void)r;return false;}
static inline bool cJSON_IsNumber(const cJSON *r){(void)r;return false;}
static inline bool cJSON_IsTrue(const cJSON *r){(void)r;return false;}
static inline void cJSON_Delete(cJSON *r){(void)r;}

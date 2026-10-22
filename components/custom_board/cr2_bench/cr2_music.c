#include "cr2_music.h"
#include "cr2_music_secret.h"
#include "esp_http_client.h"
#include "settings_manager.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "esp_random.h"
#include "cJSON.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>

typedef struct { unsigned id; bool play; } command_t;
static QueueHandle_t commands;
static SemaphoreHandle_t mutex;
static char cached[1024]="{\"available\":false,\"playing\":false,\"error\":\"Connecting to laptop music controls\"}";
static bool pending, online, succeeded;
static unsigned request_id;
static TickType_t updated;
static char error_text[160];

typedef struct { char bytes[1024]; size_t used; bool overflow; } response_t;
static esp_err_t response_event(esp_http_client_event_t *event) {
    response_t *r=event->user_data;
    if(event->event_id==HTTP_EVENT_ON_DATA) {
        if(event->data_len<0 || r->used+(size_t)event->data_len>=sizeof(r->bytes)) {r->overflow=true;return ESP_FAIL;}
        memcpy(r->bytes+r->used,event->data,event->data_len);r->used+=event->data_len;r->bytes[r->used]=0;
    }
    return ESP_OK;
}
static bool fetch_host(const command_t *command,response_t *response) {
    char host[128]={0},url[180],body[32];
    if(settings_get_server_host(host,sizeof(host))!=ESP_OK || !host[0])return false;
    // Use only the configured Snapserver host, never a browser-supplied URL.
    for(char *p=host;*p;++p)if(!isalnum((unsigned char)*p)&&*p!='.'&&*p!='-')return false;
    snprintf(url,sizeof(url),"http://%s:1781/music",host);
    esp_http_client_config_t config={.url=url,.timeout_ms=command?60000:3000,
        .event_handler=response_event,.user_data=response,.disable_auto_redirect=true};
    esp_http_client_handle_t client=esp_http_client_init(&config);
    if(!client)return false;
    esp_http_client_set_header(client,"X-Speaker-Key",CR2_MUSIC_KEY);
    if(command) {
        snprintf(body,sizeof(body),"{\"op\":\"%s\"}",command->play?"play":"pause");
        esp_http_client_set_method(client,HTTP_METHOD_POST);
        esp_http_client_set_header(client,"Content-Type","application/json");
        esp_http_client_set_post_field(client,body,strlen(body));
    }
    esp_err_t result=esp_http_client_perform(client);
    int code=esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    if(result!=ESP_OK||response->overflow)return false;
    cJSON *j=cJSON_Parse(response->bytes);
    bool json=cJSON_IsObject(j);cJSON_Delete(j);
    return json && code==200;
}
static void music_worker(void *unused) {
    (void)unused;
    for(;;) {
        command_t command={0};bool have=xQueueReceive(commands,&command,pdMS_TO_TICKS(3000))==pdTRUE;
        response_t response={0};bool ok=fetch_host(have?&command:NULL,&response);
        xSemaphoreTake(mutex,portMAX_DELAY);
        online=ok;
        if(ok){memcpy(cached,response.bytes,response.used+1);updated=xTaskGetTickCount();}
        if(have) {
            pending=false;succeeded=ok;error_text[0]=0;
            if(!ok)snprintf(error_text,sizeof(error_text),"Music command not confirmed. Check the laptop source and try again.");
        }
        xSemaphoreGive(mutex);
    }
}
static esp_err_t status_get(httpd_req_t *req) {
    char snapshot[1024],err[160];bool active,connected,success;unsigned id,age;
    xSemaphoreTake(mutex,portMAX_DELAY);
    memcpy(snapshot,cached,sizeof(snapshot));memcpy(err,error_text,sizeof(err));
    active=pending;connected=online;success=succeeded;id=request_id;
    age=(xTaskGetTickCount()-updated)*portTICK_PERIOD_MS;
    xSemaphoreGive(mutex);
    cJSON *j=cJSON_Parse(snapshot);
    if(!j)j=cJSON_CreateObject();
    cJSON_AddBoolToObject(j,"online",connected && age<15000);
    cJSON_AddBoolToObject(j,"pending",active);
    cJSON_AddBoolToObject(j,"succeeded",success);
    cJSON_AddNumberToObject(j,"request_id",id);
    cJSON_AddStringToObject(j,"command_error",err);
    char *text=cJSON_PrintUnformatted(j);cJSON_Delete(j);
    if(!text)return ESP_ERR_NO_MEM;
    httpd_resp_set_type(req,"application/json");httpd_resp_set_hdr(req,"Cache-Control","no-store");
    esp_err_t result=httpd_resp_sendstr(req,text);cJSON_free(text);return result;
}
static esp_err_t command_post(httpd_req_t *req) {
    char type[32],header[8],body[48];
    if(req->content_len<2 || req->content_len>=sizeof(body) ||
       httpd_req_get_hdr_value_str(req,"Content-Type",type,sizeof(type))!=ESP_OK || strcmp(type,"application/json") ||
       httpd_req_get_hdr_value_str(req,"X-CR2-Control",header,sizeof(header))!=ESP_OK || strcmp(header,"1"))
        return httpd_resp_send_err(req,HTTPD_400_BAD_REQUEST,"Expected play or pause JSON");
    size_t got=0;
    while(got<req->content_len) {int n=httpd_req_recv(req,body+got,req->content_len-got);if(n<=0)return ESP_FAIL;got+=n;}
    body[got]=0;
    cJSON *j=cJSON_ParseWithLengthOpts(body,got+1,NULL,true);
    cJSON *op=cJSON_GetObjectItemCaseSensitive(j,"op");
    bool valid=cJSON_IsObject(j)&&cJSON_GetArraySize(j)==1&&cJSON_IsString(op)&&
        (!strcmp(op->valuestring,"play")||!strcmp(op->valuestring,"pause"));
    if(!valid){cJSON_Delete(j);return httpd_resp_send_err(req,HTTPD_400_BAD_REQUEST,"Only play and pause are supported");}
    command_t command={.id=esp_random(),.play=!strcmp(op->valuestring,"play")};cJSON_Delete(j);
    xSemaphoreTake(mutex,portMAX_DELAY);
    bool accepted=!pending && xQueueSend(commands,&command,0)==pdTRUE;
    if(accepted){pending=true;succeeded=false;request_id=command.id;error_text[0]=0;}
    xSemaphoreGive(mutex);
    if(!accepted)return httpd_resp_send_err(req,HTTPD_400_BAD_REQUEST,"A music command is still pending");
    return status_get(req);
}
void cr2_music_register_http(httpd_handle_t server) {
    mutex=xSemaphoreCreateMutex();commands=xQueueCreate(1,sizeof(command_t));
    configASSERT(mutex && commands);
    const httpd_uri_t routes[]={
        {.uri="/cr2/music",.method=HTTP_GET,.handler=status_get},
        {.uri="/cr2/music",.method=HTTP_POST,.handler=command_post}};
    for(unsigned n=0;n<2;++n)ESP_ERROR_CHECK(httpd_register_uri_handler(server,&routes[n]));
    configASSERT(xTaskCreate(music_worker,"cr2_music",6144,NULL,2,NULL)==pdPASS);
}

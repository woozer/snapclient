#include "cr2_ota.h"
#include "cr2_ota_secret.h"  // Generated locally; never committed or logged.
#include "esp_app_desc.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

const char *cr2_ota_running_slot(void) {
    const esp_partition_t *running=esp_ota_get_running_partition();
    return running ? running->label : "unknown";
}

static bool authorized(httpd_req_t *req) {
    char token[65];
    if(httpd_req_get_hdr_value_len(req,"X-CR2-OTA-Token")!=64 ||
       httpd_req_get_hdr_value_str(req,"X-CR2-OTA-Token",token,sizeof(token))!=ESP_OK)return false;
    unsigned different=0;
    for(unsigned n=0;n<64;++n)different|=(unsigned char)token[n]^(unsigned char)CR2_OTA_TOKEN[n];
    memset(token,0,sizeof(token));return different==0;
}

static void restart(void *unused) {
    (void)unused;vTaskDelay(pdMS_TO_TICKS(500));esp_restart();
}

static esp_err_t upload(httpd_req_t *req) {
    if(!authorized(req))return httpd_resp_send_err(req,HTTPD_401_UNAUTHORIZED,"OTA token required");
    const esp_partition_t *target=esp_ota_get_next_update_partition(NULL);
    const esp_partition_t *running=esp_ota_get_running_partition();
    if(!target || !running || target->address==running->address ||
       req->content_len<4096 || req->content_len>target->size)
        return httpd_resp_send_err(req,HTTPD_400_BAD_REQUEST,"Invalid app image size or inactive slot");
    if(!cr2_audio_begin_update())
        return httpd_resp_send_err(req,HTTPD_400_BAD_REQUEST,"Disarm audio; fresh healthy power reading required");

    esp_ota_handle_t handle=0;
    bool begun=false;
    esp_err_t err=esp_ota_begin(target,req->content_len,&handle);
    if(err!=ESP_OK)goto failed;
    begun=true;
    char buffer[2048];size_t received=0;
    int64_t deadline=esp_timer_get_time()+120000000;
    while(received<req->content_len) {
        if(esp_timer_get_time()>deadline)goto failed;
        size_t remaining=req->content_len-received;
        int count=httpd_req_recv(req,buffer,remaining<sizeof(buffer)?remaining:sizeof(buffer));
        if(count<=0)goto failed;
        if(esp_ota_write(handle,buffer,count)!=ESP_OK)goto failed;
        received+=(size_t)count;
    }
    err=esp_ota_end(handle);begun=false;
    if(err!=ESP_OK)goto failed; // IDF validates the complete ESP image.
    esp_app_desc_t description;
    if(esp_ota_get_partition_description(target,&description)!=ESP_OK ||
       strncmp(description.project_name,"snapclient",sizeof(description.project_name))!=0 ||
       strncmp(description.version,"c-r2-play-",10)!=0)goto failed;
    // Boot selection changes only after the whole compatible image is valid.
    if(esp_ota_set_boot_partition(target)!=ESP_OK)goto failed;
    httpd_resp_set_type(req,"application/json");
    httpd_resp_set_hdr(req,"Cache-Control","no-store");
    esp_err_t response=httpd_resp_sendstr(req,"{\"accepted\":true,\"rebooting\":true}");
    // Keep update interlock/amp OFF until reboot; never restore previous audio.
    if(xTaskCreate(restart,"cr2_restart",2048,NULL,3,NULL)!=pdPASS)esp_restart();
    return response;
failed:
    if(begun)esp_ota_abort(handle);
    cr2_audio_end_update();
    return httpd_resp_send_err(req,HTTPD_400_BAD_REQUEST,"OTA failed; previous boot selection retained; audio OFF");
}

void cr2_ota_register_http(httpd_handle_t server) {
    const httpd_uri_t route={.uri="/cr2/firmware",.method=HTTP_POST,.handler=upload};
    ESP_ERROR_CHECK(httpd_register_uri_handler(server,&route));
}

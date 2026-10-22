#include "cr2_bench.h"
#include "cr2_sync_probe.h"
#include "cr2_dma_probe.h"
#include "cr2_trace.h"
#include "cr2_power_policy.h"
#include "cr2_ota.h"
#include "cr2_music.h"
#include "driver/gpio.h"
#include "driver/i2c.h"
#include "esp_log.h"
#include "esp_psram.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_random.h"
#include "cJSON.h"
#include "cr2_web_assets.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

#define CHARGE GPIO_NUM_21
#define PDN GPIO_NUM_14
#define SELECTOR GPIO_NUM_11
#define INPUT_SELECT GPIO_NUM_10
#define TAS 0x4c
#define BQ 0x6b
_Static_assert(CONFIG_MASTER_I2S_MCLK_PIN == -1, "No MCLK");
_Static_assert(CONFIG_MASTER_I2S_BCK_PIN == 17, "BCLK17");
_Static_assert(CONFIG_MASTER_I2S_LRCK_PIN == 15, "LRCLK15");
_Static_assert(CONFIG_MASTER_I2S_DATAOUT_PIN == 16, "DATA16");
_Static_assert(configTICK_RATE_HZ == 1000, "Millisecond safety clock");

static const char *TAG="CR2_AUDIO";
static atomic_bool clock_on, format_ok, enabled, muted=true, faulted, playing, arm_active;
static atomic_uint lease_until, last_write, worker_tick;
static atomic_uint clock_epoch, clock_since, resyncs, last_resync, empty_waits, underruns;
static atomic_int last_rssi, wifi_channel;
static atomic_uint bssid_lo,bssid_hi;
static atomic_uint ui_session, control_pending;
static atomic_uint pd_mv,pd_ma,pd_status0,pd_status1;
static atomic_bool pd_valid;
static atomic_int ui_input_st,ui_audio_st;
static atomic_uint ui_pd_mv,ui_pd_ma,ui_analog=31,ui_ceiling=96;
static atomic_uint control_value=40|256; // volume40, muted; never auto-arm.
#define CONTROL_VALID 512u
bool cr2_audio_take_control(int *volume, bool *muted_out) {
    unsigned c=atomic_exchange(&control_pending,0);
    if(!(c&CONTROL_VALID))return false;
    *volume=c&255;*muted_out=!!(c&256);return true;
}
void cr2_audio_server_settings(int *volume, bool *muted_out) {
    if(atomic_load(&ui_session)) {
        unsigned c=atomic_load(&control_value);
        *volume=c&255;*muted_out=!!(c&256);
    }
    // Server controls stay independent when no browser session is active.
}
static void queue_control(int volume, bool mute) {
    unsigned c=(unsigned)volume|(mute?256u:0u);
    atomic_store(&control_value,c);atomic_store(&control_pending,c|CONTROL_VALID);
}
static atomic_int volume_value, vbus_mv, sys_mv, ibus_ma, vbat_mv, die_half_c;
static atomic_int bq_fault0, bq_fault1;
static atomic_bool power_valid;
static atomic_int power_profile;
static atomic_bool update_active;
static atomic_uint power_sample_ms, power_attempt_ms, power_sequence;
static esp_timer_handle_t guard_timer;
static bool bus_ready;

static uint32_t now_ms(void) {return xTaskGetTickCount();}
static bool armed(void) {return atomic_load(&arm_active) && (int32_t)(atomic_load(&lease_until)-now_ms())>0;}
static void off(void) {gpio_set_level(PDN,0);atomic_store(&playing,false);}
static bool desired(void) {
    if(atomic_load(&ui_session)) {
        unsigned c=atomic_load(&control_value);
        if(c&256)return false;
        if(!atomic_load(&playing) && (atomic_load(&control_pending) ||
           atomic_load(&volume_value)!=(int)(c&255)))return false;
    }
    return armed() && !atomic_load(&update_active) && !atomic_load(&faulted) && atomic_load(&clock_on) &&
        cr2_selector_allowed(atomic_load(&power_profile),gpio_get_level(SELECTOR)) &&
        atomic_load(&format_ok) && atomic_load(&enabled) && !atomic_load(&muted) &&
        atomic_load(&volume_value)>0 && now_ms()-atomic_load(&last_write)<250;
}
void cr2_audio_clock(bool running) {
    bool old=atomic_exchange(&clock_on,running);
    if(old!=running) {
        atomic_fetch_add(&clock_epoch,1);
        atomic_store(&clock_since,now_ms());
    }
    if(!running) off(); // Synchronous shutdown BEFORE the player stops clocks.
}
void cr2_audio_resync(int kind) {
    (void)kind;atomic_fetch_add(&resyncs,1);atomic_store(&last_resync,now_ms());
}
void cr2_audio_buffer(bool recovered) {
    if(recovered)atomic_fetch_add(&empty_waits,1);
    else atomic_fetch_add(&underruns,1);
}
void cr2_audio_written(void) {atomic_store(&last_write,now_ms());}
void cr2_audio_format(int rate,int bits,int channels) {
    off();
    atomic_store(&format_ok,rate==48000 && bits==16 && channels==2);
    ESP_LOGI(TAG,"FORMAT rate=%d bits=%d channels=%d accepted=%d",rate,bits,channels,atomic_load(&format_ok));
}
static void guard(void *unused) {
    (void)unused;
    if(atomic_load(&ui_session) && armed() &&
       (gpio_get_level(INPUT_SELECT)!=atomic_load(&ui_input_st) || gpio_get_level(SELECTOR)!=atomic_load(&ui_audio_st))) {
        off();atomic_store(&arm_active,false);atomic_store(&ui_session,0);atomic_store(&faulted,true);
    }
    if(atomic_load(&arm_active) && !armed()) {
        atomic_store(&arm_active,false);atomic_store(&ui_session,0);
    }
    if(!desired() || now_ms()-atomic_load(&worker_tick)>1500) off();
    if(gpio_get_level(CHARGE)) {off();atomic_store(&faulted,true);}
    // A dock allowance must never survive fallback to USB/SYS. Require a
    // new explicit arm after any selector mismatch, even if dock returns.
    if(armed() && !cr2_selector_allowed(atomic_load(&power_profile),gpio_get_level(SELECTOR))) {
        off();atomic_store(&arm_active,false);atomic_store(&ui_session,0);atomic_store(&faulted,true);
    }
}
static bool read_regs(uint8_t address,uint8_t reg,uint8_t *out,size_t size) {
    return bus_ready && i2c_master_write_read_device(I2C_NUM_0,address,&reg,1,out,size,pdMS_TO_TICKS(25))==ESP_OK;
}
static bool write_reg(uint8_t address,uint8_t reg,uint8_t value) {
    uint8_t data[]={reg,value};
    return bus_ready && i2c_master_write_to_device(I2C_NUM_0,address,data,2,pdMS_TO_TICKS(25))==ESP_OK;
}
static bool tas_write(uint8_t reg,uint8_t value) {
    uint8_t got=0;
    return desired() && gpio_get_level(PDN) && write_reg(TAS,reg,value) &&
        read_regs(TAS,reg,&got,1) && got==value;
}
static bool bq_word(uint8_t reg,uint16_t *v) {
    uint8_t d[2];if(!read_regs(BQ,reg,d,2))return false;
    *v=((uint16_t)d[0]<<8)|d[1];return true;
}
static bool power(void) {
    // Only this worker touches the I2C bus. Idle telemetry never enables audio
    // or charging; playback gates use the explicitly armed source profile.
    atomic_fetch_add(&power_sequence,1);
    atomic_store(&power_valid,false);
    atomic_store(&power_attempt_ms,now_ms());
    uint8_t id=0,saved=0,disabled=0;
    if(!read_regs(BQ,0x48,&id,1) || ((id>>3)&7)!=1 ||
       !read_regs(BQ,0x2e,&saved,1) || !read_regs(BQ,0x2f,&disabled,1) || (disabled&0xba)) {
        atomic_fetch_add(&power_sequence,1);return false;
    }
    bool done=false;
    if(write_reg(BQ,0x2e,(saved&3)|0xc0)) {
        for(unsigned n=0;n<100;++n) {
            vTaskDelay(pdMS_TO_TICKS(10));uint8_t ctrl=0;
            if(!read_regs(BQ,0x2e,&ctrl,1))break;
            if(!(ctrl&0x80)){done=true;break;}
        }
    }
    uint16_t vb=0,vs=0,ib=0,bat=0,die=0;uint8_t faults[2]={0};
    bool ok=done && bq_word(0x35,&vb) && bq_word(0x3d,&vs) && bq_word(0x31,&ib) &&
        bq_word(0x3b,&bat) && bq_word(0x41,&die) && read_regs(BQ,0x20,faults,2);
    bool restored=write_reg(BQ,0x2e,saved);ok=ok&&restored;
    atomic_store(&vbus_mv,vb);atomic_store(&sys_mv,vs);atomic_store(&ibus_ma,(int16_t)ib);
    atomic_store(&vbat_mv,bat);atomic_store(&die_half_c,(int16_t)die);
    atomic_store(&bq_fault0,faults[0]);atomic_store(&bq_fault1,faults[1]);
    // Never probe the USB-powered HUSB while battery-only. GPIO10 LOW means
    // U102 IN2 (USB); HIGH means pogo OR Hi-Z, so also require measured VBUS.
    uint8_t p0=0,p1=0,offer=0,check=0;
    bool pdok=false;
    if(ok && vb>=4500 && vb<=20500 && gpio_get_level(INPUT_SELECT)==0 &&
       read_regs(0x08,0,&p0,1) && read_regs(0x08,1,&p1,1) && cr2_pd_mv(p0) &&
       read_regs(0x08,(p0>>4)+1,&offer,1) && read_regs(0x08,0,&check,1) && check==p0)
        pdok=cr2_pd_matches(p0,p1,offer,vb,gpio_get_level(INPUT_SELECT));
    atomic_store(&pd_status0,p0);atomic_store(&pd_status1,p1);
    atomic_store(&pd_mv,pdok?cr2_pd_mv(p0):0);atomic_store(&pd_ma,pdok?cr2_pd_ma(p0):0);
    atomic_store(&pd_valid,pdok);
    if(ok)atomic_store(&power_sample_ms,now_ms());
    atomic_store(&power_valid,ok);
    atomic_fetch_add(&power_sequence,1);
    ESP_LOGI(TAG,"POWER read=%d VBUS=%umV VBAT=%umV SYS=%umV IBUS=%dmA selector=%d charge=%d",ok,vb,bat,vs,(int16_t)ib,gpio_get_level(SELECTOR),gpio_get_level(CHARGE));
    bool contract_ok=atomic_load(&power_profile)!=CR2_USB_PD ||
        (pdok && atomic_load(&ui_pd_mv)==cr2_pd_mv(p0) && atomic_load(&ui_pd_ma)==cr2_pd_ma(p0) &&
         (int16_t)ib<=(int)(cr2_pd_ma(p0)*8/10));
    return contract_ok && cr2_supply_allowed(atomic_load(&power_profile),ok,vb,vs,(int16_t)ib,
        gpio_get_level(SELECTOR),gpio_get_level(CHARGE),bat,faults[0],faults[1],(int16_t)die);
}
static uint8_t gain_register(void) {
    int volume=atomic_load(&volume_value);
    if(!atomic_load(&ui_session))return cr2_limited_gain(atomic_load(&power_profile),volume);
    if(volume<=0)return 255;
    unsigned gain=atomic_load(&ui_ceiling)+(unsigned)ceil(1.2*(100-volume));
    return gain>254?254:gain;
}
static uint8_t analog_register(void) {
    return atomic_load(&ui_session)?atomic_load(&ui_analog):31;
}
static bool tas_status(uint8_t control,uint8_t state,bool verbose) {
    uint8_t c[3]={0},f[4]={0},actual=0,s=0;
    bool ok=read_regs(TAS,0x37,c,3)&&read_regs(TAS,0x70,f,4)&&
        read_regs(TAS,3,&actual,1)&&read_regs(TAS,0x68,&s,1);
    unsigned ratio=((c[0]&0x30)<<4)|c[1];
    ok=ok&&(c[0]&15)==9&&ratio==32&&(c[2]&0x3f)==8&&actual==control&&(s&3)==state&&
        !f[0]&&!f[1]&&!f[2]&&!f[3];
    if(verbose||!ok)ESP_LOGI(TAG,"TAS ok=%d ctrl=%02X state=%02X clocks=%02X/%02X/%02X faults=%02X/%02X/%02X/%02X",ok,actual,s,c[0],c[1],c[2],f[0],f[1],f[2],f[3]);
    return ok;
}
static bool start_amp(void) {
    off();
    if(!power()||!desired())return false;
    vTaskDelay(pdMS_TO_TICKS(10));
    if(!desired())return false;
    gpio_set_level(PDN,1);vTaskDelay(pdMS_TO_TICKS(10));
    uint8_t id=0,ctrl=0;
    if(!tas_write(0,0)||!tas_write(0x7f,0)||!read_regs(TAS,0x67,&id,1)||id!=0x95||
       !read_regs(TAS,3,&ctrl,1)||ctrl!=0x10)return false;
    // Same ROM1/ZROM setup that passed standalone playback;16-bit standard I2S.
    if(!tas_write(3,0x18)||!tas_write(2,0)||!tas_write(0x33,0)||!tas_write(0x35,0x11)||
       !tas_write(0x40,1)||!tas_write(0x46,1)||!tas_write(0x4c,gain_register())||!tas_write(0x54,analog_register()))return false;
    if(!tas_write(3,0x0a))return false;
    vTaskDelay(pdMS_TO_TICKS(20));
    if(!tas_status(0x0a,2,true)||!power())return false;
    if(!tas_write(0x4c,gain_register())||!tas_write(0x54,analog_register())||!tas_write(3,0x0b))return false;
    vTaskDelay(pdMS_TO_TICKS(20));
    if(!tas_status(0x0b,3,true)||!power()||!tas_write(3,3))return false;
    atomic_store(&playing,true);
    ESP_LOGI(TAG,"PLAY volume=%d digital_reg=%02X analog=%.1fdB ceiling=%.1fdB profile=%s",atomic_load(&volume_value),gain_register(),-0.5*analog_register(),atomic_load(&ui_session)?24.0-0.5*atomic_load(&ui_ceiling):cr2_ceiling_db(atomic_load(&power_profile)),cr2_profile_name(atomic_load(&power_profile)));
    return true;
}
static void fail(void) {
    off();atomic_store(&faulted,true);atomic_store(&arm_active,false);atomic_store(&ui_session,0);
    ESP_LOGE(TAG,"FAULT latched; ampOFF, re-arm required; no fault registers cleared/masked");
}
static void worker(void *unused) {
    (void)unused;uint32_t health=0,power_at=0;uint8_t last_gain=0xff;
    for(;;) {
        atomic_store(&worker_tick,now_ms());
        if(!desired()) {
            off();
            if(now_ms()-atomic_load(&power_attempt_ms)>=2000) {
                // Read fresh rails while disarmed, including at dock voltage.
                // A false USB playback gate here does not latch an amp fault.
                (void)power();
            }
        }
        else if(!gpio_get_level(PDN)||!atomic_load(&playing)) {
            // Let clocks settle before the roughly1s amplifier/ADC sequence.
            if(now_ms()-atomic_load(&clock_since)>=500) {
                unsigned epoch=atomic_load(&clock_epoch);
                if(!start_amp()){off();if(desired()&&epoch==atomic_load(&clock_epoch))fail();}
                else {last_gain=gain_register();power_at=now_ms();}
            }
        } else {
            unsigned epoch=atomic_load(&clock_epoch);
            bool ok=tas_status(3,3,false);
            uint8_t gain=gain_register();
            if(ok&&gain!=last_gain){
                // Ramp increasing levels by at most1dB per worker iteration.
                // Reductions/mute remain immediate. Hardware also interpolates.
                if(gain+2<last_gain)gain=last_gain-2;
                ok=tas_write(0x4c,gain);last_gain=gain;
            }
            if(ok&&now_ms()-power_at>=2000){ok=power();power_at=now_ms();}
            if(!ok){off();if(desired()&&epoch==atomic_load(&clock_epoch))fail();}
        }
        if(now_ms()-health>=1000) {
            health=now_ms();wifi_ap_record_t ap={0};
            esp_err_t w=esp_wifi_sta_get_ap_info(&ap);
            atomic_store(&last_rssi,w==ESP_OK?ap.rssi:0);
            atomic_store(&wifi_channel,w==ESP_OK?ap.primary:0);
            atomic_store(&bssid_lo,(unsigned)ap.bssid[0]|((unsigned)ap.bssid[1]<<8)|((unsigned)ap.bssid[2]<<16)|((unsigned)ap.bssid[3]<<24));
            atomic_store(&bssid_hi,(unsigned)ap.bssid[4]|((unsigned)ap.bssid[5]<<8));
            wifi_ps_type_t ps;int8_t tx;
            int pm=esp_wifi_get_ps(&ps)==ESP_OK?(int)ps:-1;
            int tm=esp_wifi_get_max_tx_power(&tx)==ESP_OK?(int)tx:-1;
            cr2_trace(8,w==ESP_OK?ap.rssi:0,pm,tm,w);
            if(w!=ESP_OK)off();
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}
static esp_err_t http_status(httpd_req_t *req) {
    char out[2048];
    unsigned seq=atomic_load(&power_sequence),sample=atomic_load(&power_sample_ms);
    bool valid=atomic_load(&power_valid);
    int vb=atomic_load(&vbus_mv),vs=atomic_load(&sys_mv),ib=atomic_load(&ibus_ma);
    int bat=atomic_load(&vbat_mv),die=atomic_load(&die_half_c);
    int f0=atomic_load(&bq_fault0),f1=atomic_load(&bq_fault1);
    valid=valid && !(seq&1) && seq==atomic_load(&power_sequence);
    unsigned age=now_ms()-sample;
    snprintf(out,sizeof(out),"{\"firmware\":\"c-r2-play-0.2.20-wait\",\"playback_variant\":\"block240-bounded-dma-wait\",\"dma_monitor_enabled\":false,\"armed\":%s,\"amp\":%d,\"charge\":%d,\"playing\":%s,\"fault\":%s,\"clock\":%s,\"format_ok\":%s,\"volume\":%d,\"muted\":%s,\"vbus_mv\":%d,\"sys_mv\":%d,\"ibus_ma\":%d,\"rssi_dbm\":%d,\"resyncs\":%u,\"resync_age_ms\":%u,\"empty_waits\":%u,\"dma_underruns\":%u,\"power_valid\":%s,\"power_fresh\":%s,\"power_sequence\":%u,\"power_age_ms\":%d,\"selector\":%d,\"power_profile\":\"%s\",\"digital_ceiling_db\":%.1f,\"vbat_mv\":%d,\"die_half_c\":%d,\"bq_fault0\":%d,\"bq_fault1\":%d,\"ota_available\":true,\"ota_active\":%s,\"ota_slot\":\"%s\",\"uptime_ms\":%u}",
        armed()?"true":"false",gpio_get_level(PDN),gpio_get_level(CHARGE),atomic_load(&playing)?"true":"false",atomic_load(&faulted)?"true":"false",atomic_load(&clock_on)?"true":"false",atomic_load(&format_ok)?"true":"false",atomic_load(&volume_value),atomic_load(&muted)?"true":"false",vb,vs,ib,atomic_load(&last_rssi),atomic_load(&resyncs),(unsigned)(now_ms()-atomic_load(&last_resync)),atomic_load(&empty_waits),atomic_load(&underruns),valid?"true":"false",(valid&&age<=3000)?"true":"false",seq,sample?(int)age:-1,gpio_get_level(SELECTOR),cr2_profile_name(atomic_load(&power_profile)),atomic_load(&ui_session)?24.0-0.5*atomic_load(&ui_ceiling):cr2_ceiling_db(atomic_load(&power_profile)),bat,die,f0,f1,atomic_load(&update_active)?"true":"false",cr2_ota_running_slot(),(unsigned)now_ms());
    size_t used=strlen(out);
    unsigned lo=atomic_load(&bssid_lo),hi=atomic_load(&bssid_hi),cv=atomic_load(&control_value);
    snprintf(out+used-1,sizeof(out)-used+1,
        ",\"ui_active\":%s,\"control_volume\":%u,\"control_muted\":%s,\"lease_ms\":%u,\"wifi_channel\":%d,\"bssid\":\"%02x:%02x:%02x:%02x:%02x:%02x\",\"analog_gain_db\":%.1f,\"ui_ceiling_db\":%.1f,\"input_selector\":%d,\"pd_valid\":%s,\"pd_mv\":%u,\"pd_ma\":%u,\"pd_status0\":%u,\"pd_status1\":%u}",
        atomic_load(&ui_session)&&armed()?"true":"false",cv&255,cv&256?"true":"false",
        armed()?(unsigned)(atomic_load(&lease_until)-now_ms()):0,atomic_load(&wifi_channel),
        lo&255,(lo>>8)&255,(lo>>16)&255,(lo>>24)&255,hi&255,(hi>>8)&255,
        -0.5*analog_register(),24.0-0.5*atomic_load(&ui_ceiling),
        gpio_get_level(INPUT_SELECT),atomic_load(&pd_valid)?"true":"false",atomic_load(&pd_mv),atomic_load(&pd_ma),atomic_load(&pd_status0),atomic_load(&pd_status1));
    httpd_resp_set_type(req,"application/json");httpd_resp_set_hdr(req,"Cache-Control","no-store");
    return httpd_resp_sendstr(req,out);
}
static esp_err_t http_arm_profile(httpd_req_t *req, cr2_power_profile_t profile) {
    if(atomic_load(&update_active))return httpd_resp_send_err(req,HTTPD_400_BAD_REQUEST,"Firmware update active");
    atomic_store(&ui_session,0);atomic_store(&arm_active,false);off();atomic_store(&faulted,false);
    atomic_store(&power_profile,profile);
    unsigned lease=profile==CR2_BATTERY4S ? 90000 : 120000;
    atomic_store(&lease_until,now_ms()+lease);atomic_store(&arm_active,true);
    ESP_LOGI(TAG,"Explicit playback arm for%ums profile=%s, charge remainsLOW",lease,cr2_profile_name(profile));return http_status(req);
}
static esp_err_t http_arm(httpd_req_t *req) {return http_arm_profile(req,CR2_USB5);}
static esp_err_t http_arm_dock(httpd_req_t *req) {return http_arm_profile(req,CR2_DOCK19);}
static esp_err_t http_arm_battery(httpd_req_t *req) {return http_arm_profile(req,CR2_BATTERY4S);}
static esp_err_t http_stop(httpd_req_t *req) {
    atomic_store(&ui_session,0);queue_control(atomic_load(&control_value)&255,true);atomic_store(&arm_active,false);off();ESP_LOGI(TAG,"Explicit stop");return http_status(req);
}

// GETs never change output state. A custom header + JSON body prevents simple
// cross-origin forms from arming audio; no CORS permission is advertised.
static esp_err_t http_control(httpd_req_t *req) {
    char type[48],header[8],body[192];
    if(httpd_req_get_hdr_value_str(req,"Content-Type",type,sizeof(type))!=ESP_OK ||
       strcmp(type,"application/json") ||
       httpd_req_get_hdr_value_str(req,"X-CR2-Control",header,sizeof(header))!=ESP_OK ||
       strcmp(header,"1") || req->content_len<2 || req->content_len>=sizeof(body))
        return httpd_resp_send_err(req,HTTPD_400_BAD_REQUEST,"Expected bounded JSON control request");
    size_t have=0;
    while(have<req->content_len) {
        int n=httpd_req_recv(req,body+have,req->content_len-have);
        if(n<=0)return httpd_resp_send_err(req,HTTPD_400_BAD_REQUEST,"Incomplete command");
        have+=(size_t)n;
    }
    body[have]=0;
    const char *end=NULL;
    cJSON *j=cJSON_ParseWithLengthOpts(body,have+1,&end,true);
    if(!cJSON_IsObject(j)){cJSON_Delete(j);return httpd_resp_send_err(req,HTTPD_400_BAD_REQUEST,"Invalid JSON object");}
    cJSON *op=cJSON_GetObjectItemCaseSensitive(j,"op");
    cJSON *vol=cJSON_GetObjectItemCaseSensitive(j,"volume");
    cJSON *mu=cJSON_GetObjectItemCaseSensitive(j,"muted");
    cJSON *token=cJSON_GetObjectItemCaseSensitive(j,"session");
    const char *err=NULL;
    unsigned seen=0;
    for(cJSON *item=j->child;item;item=item->next) {
        unsigned bit=!strcmp(item->string,"op")?1:!strcmp(item->string,"volume")?2:
            !strcmp(item->string,"muted")?4:!strcmp(item->string,"session")?8:0;
        if(!bit || (seen&bit))err="Unknown or duplicate field";
        seen|=bit;
    }
    if(!cJSON_IsString(op))err="Missing operation";
    if(vol&&(!cJSON_IsNumber(vol)||!isfinite(vol->valuedouble)||vol->valuedouble<0||vol->valuedouble>100||floor(vol->valuedouble)!=vol->valuedouble))err="Volume must be an integer 0..100";
    if(mu&&!cJSON_IsBool(mu))err="Muted must be boolean";
    if(token&&(!cJSON_IsNumber(token)||token->valuedouble<1||token->valuedouble>4294967295.0||floor(token->valuedouble)!=token->valuedouble))err="Invalid session";
    if(err){cJSON_Delete(j);return httpd_resp_send_err(req,HTTPD_400_BAD_REQUEST,err);}
    const char *name=op->valuestring;
    if(!strcmp(name,"stop")) {
        cJSON_Delete(j);return http_stop(req);
    }
    if(atomic_load(&update_active))err="Update active";
    else if(!strcmp(name,"start")) {
        if(atomic_load(&ui_session)&&armed())err="A controller is already active; use Standby first";
        else if(!vol||vol->valueint>50||mu||token)err="Start requires volume 0..50 only";
        else {
            unsigned seq=atomic_load(&power_sequence);
            unsigned usb_mv=atomic_load(&pd_mv),usb_ma=atomic_load(&pd_ma);
            int input_st=gpio_get_level(INPUT_SELECT),audio_st=gpio_get_level(SELECTOR);
            int profile=-1;
            for(int p=0;p<4;++p) {
                if(p==CR2_DOCK19 && gpio_get_level(INPUT_SELECT)!=1)continue;
                if(p==CR2_USB_PD && (!atomic_load(&pd_valid)||input_st!=0||!usb_mv||!usb_ma))continue;
                if(cr2_supply_allowed(p,atomic_load(&power_valid),atomic_load(&vbus_mv),
                    atomic_load(&sys_mv),atomic_load(&ibus_ma),gpio_get_level(SELECTOR),
                    gpio_get_level(CHARGE),atomic_load(&vbat_mv),atomic_load(&bq_fault0),
                    atomic_load(&bq_fault1),atomic_load(&die_half_c)))profile=p;
            }
            if(profile<0 || now_ms()-atomic_load(&power_sample_ms)>3000 || (seq&1) || seq!=atomic_load(&power_sequence))err="Supply not ready; retry when power readings are fresh";
            else {
                atomic_store(&arm_active,false);off();
                atomic_store(&power_profile,profile);atomic_store(&faulted,false);
                atomic_store(&ui_input_st,input_st);atomic_store(&ui_audio_st,audio_st);
                atomic_store(&ui_pd_mv,usb_mv);atomic_store(&ui_pd_ma,usb_ma);
                atomic_store(&ui_analog,profile==CR2_USB_PD?cr2_pd_analog(usb_mv):cr2_ui_analog(profile));
                atomic_store(&ui_ceiling,profile==CR2_USB_PD?cr2_pd_ceiling_reg(usb_mv,usb_ma):cr2_ui_gain(profile,100));
                unsigned session;do{session=esp_random();}while(!session);
                atomic_store(&ui_session,session);
                queue_control(vol->valueint,false);
                atomic_store(&lease_until,now_ms()+90000);atomic_store(&arm_active,true);
                char reply[64];snprintf(reply,sizeof(reply),"{\"session\":%u}",session);
                cJSON_Delete(j);httpd_resp_set_type(req,"application/json");
                httpd_resp_set_hdr(req,"Cache-Control","no-store");
                return httpd_resp_sendstr(req,reply);
            }
        }
    } else if(!strcmp(name,"renew")||!strcmp(name,"set")) {
        unsigned session=atomic_load(&ui_session);
        if(!token||!session||(uint32_t)token->valuedouble!=session||!armed()||atomic_load(&faulted))err="Session expired; explicitly start again";
        else if(!strcmp(name,"renew")) {
            if(vol||mu)err="Renew accepts only session";
            else atomic_store(&lease_until,now_ms()+90000);
        } else if(!vol&&!mu)err="Set requires volume or muted";
        else {
            unsigned c=atomic_load(&control_value);
            int v=vol?vol->valueint:(int)(c&255);
            bool m=mu?cJSON_IsTrue(mu):!!(c&256);
            if(m)atomic_store(&muted,true);
            if(m||!v)off();
            queue_control(v,m);
        }
    } else err="Unknown operation";
    cJSON_Delete(j);
    if(err)return httpd_resp_send_err(req,HTTPD_400_BAD_REQUEST,err);
    return http_status(req);
}
static esp_err_t http_page(httpd_req_t *req) {
    httpd_resp_set_type(req,"text/html; charset=utf-8");
    httpd_resp_set_hdr(req,"Cache-Control","no-store");
    httpd_resp_set_hdr(req,"Content-Security-Policy","default-src 'self'; script-src 'self'; style-src 'self'; connect-src 'self'; frame-ancestors 'none'");
    return httpd_resp_sendstr(req,cr2_web_html);
}
static esp_err_t http_script(httpd_req_t *req) {
    httpd_resp_set_type(req,"text/javascript; charset=utf-8");
    httpd_resp_set_hdr(req,"Cache-Control","no-store");return httpd_resp_sendstr(req,cr2_web_js);
}
static esp_err_t http_style(httpd_req_t *req) {
    httpd_resp_set_type(req,"text/css; charset=utf-8");
    httpd_resp_set_hdr(req,"Cache-Control","no-store");return httpd_resp_sendstr(req,cr2_web_css);
}
bool cr2_sync_can_configure(void) {
    return !armed() && !atomic_load(&ui_session) && !atomic_load(&update_active) &&
        !atomic_load(&faulted) && !gpio_get_level(PDN) && !gpio_get_level(CHARGE);
}
void cr2_audio_register_http(httpd_handle_t server) {
    const httpd_uri_t routes[]={
        {.uri="/control",.method=HTTP_GET,.handler=http_page},
        {.uri="/control.js",.method=HTTP_GET,.handler=http_script},
        {.uri="/control.css",.method=HTTP_GET,.handler=http_style},
        {.uri="/cr2/control",.method=HTTP_POST,.handler=http_control},
        {.uri="/cr2/audio",.method=HTTP_GET,.handler=http_status},
        {.uri="/cr2/audio/arm",.method=HTTP_POST,.handler=http_arm},
        {.uri="/cr2/audio/arm-dock",.method=HTTP_POST,.handler=http_arm_dock},
        {.uri="/cr2/audio/arm-battery",.method=HTTP_POST,.handler=http_arm_battery},
        {.uri="/cr2/audio/stop",.method=HTTP_POST,.handler=http_stop}};
    for(unsigned n=0;n<sizeof(routes)/sizeof(routes[0]);++n)ESP_ERROR_CHECK(httpd_register_uri_handler(server,&routes[n]));
    cr2_ota_register_http(server);
    cr2_music_register_http(server);
    cr2_sync_register_http(server);
    cr2_dma_probe_register_http(server);
}
bool cr2_audio_begin_update(void) {
    if(atomic_exchange(&update_active,true))return false;
    unsigned seq=atomic_load(&power_sequence);
    bool ok=!armed() && !gpio_get_level(PDN) && !gpio_get_level(CHARGE) &&
        atomic_load(&power_valid) && now_ms()-atomic_load(&power_sample_ms)<=3000 &&
        !atomic_load(&bq_fault0) && !atomic_load(&bq_fault1) &&
        (atomic_load(&vbat_mv)>=14000 || atomic_load(&vbus_mv)>=4500);
    ok=ok && !(seq&1) && seq==atomic_load(&power_sequence);
    if(!ok){atomic_store(&update_active,false);return false;}
    atomic_store(&arm_active,false);off();return true;
}
void cr2_audio_end_update(void) {off();atomic_store(&arm_active,false);atomic_store(&update_active,false);}
void cr2_bench_early_init(void) {
    const gpio_num_t outputs[]={CHARGE,PDN,GPIO_NUM_17,GPIO_NUM_15,GPIO_NUM_16};
    for(unsigned i=0;i<5;++i){gpio_set_level(outputs[i],0);gpio_hold_dis(outputs[i]);gpio_set_direction(outputs[i],GPIO_MODE_INPUT_OUTPUT);gpio_set_level(outputs[i],0);}
    ESP_ERROR_CHECK(gpio_hold_en(CHARGE));gpio_set_direction(SELECTOR,GPIO_MODE_INPUT);gpio_set_direction(INPUT_SELECT,GPIO_MODE_INPUT);
    if(esp_psram_get_size()!=8*1024*1024)abort();
    cr2_trace_init();
    ESP_LOGI(TAG,"c-r2-play-0.2.20-wait bootDISARMED ampOFF chargeLOW, USB/dock/battery caps, authenticated OTA, no PD requests");
}
static esp_err_t init(audio_hal_codec_config_t *cfg) {
    (void)cfg;off();
    i2c_config_t c={.mode=I2C_MODE_MASTER,.sda_io_num=8,.scl_io_num=9,
        .sda_pullup_en=GPIO_PULLUP_DISABLE,.scl_pullup_en=GPIO_PULLUP_DISABLE,.master.clk_speed=100000};
    if(i2c_param_config(I2C_NUM_0,&c)!=ESP_OK||i2c_driver_install(I2C_NUM_0,I2C_MODE_MASTER,0,0,0)!=ESP_OK)return ESP_FAIL;
    bus_ready=true;atomic_store(&worker_tick,now_ms());
    esp_timer_create_args_t timer={.callback=guard,.name="cr2_guard"};
    if(esp_timer_create(&timer,&guard_timer)!=ESP_OK||esp_timer_start_periodic(guard_timer,20000)!=ESP_OK)return ESP_FAIL;
    if(xTaskCreate(worker,"cr2_amp",4096,NULL,3,NULL)!=pdPASS)return ESP_FAIL;
    return ESP_OK;
}
static esp_err_t deinit(void){atomic_store(&arm_active,false);off();return ESP_OK;}
static esp_err_t control(audio_hal_codec_mode_t m,audio_hal_ctrl_t s){(void)m;atomic_store(&enabled,s==AUDIO_HAL_CTRL_START);if(s!=AUDIO_HAL_CTRL_START)off();return ESP_OK;}
static esp_err_t interface(audio_hal_codec_mode_t m,audio_hal_codec_i2s_iface_t *i){(void)m;(void)i;return ESP_OK;}
static esp_err_t mute(bool on){atomic_store(&muted,on);if(on)off();return ESP_OK;}
static esp_err_t volume(int v){if(v<0)v=0;if(v>100)v=100;atomic_store(&volume_value,v);if(!v)off();return ESP_OK;}
static esp_err_t get_volume(int *v){if(!v)return ESP_ERR_INVALID_ARG;*v=atomic_load(&volume_value);return ESP_OK;}
audio_hal_func_t AUDIO_CODEC_CR2_BENCH_DEFAULT_HANDLE={
    .audio_codec_initialize=init,.audio_codec_deinitialize=deinit,.audio_codec_ctrl=control,
    .audio_codec_config_iface=interface,.audio_codec_set_mute=mute,.audio_codec_set_volume=volume,
    .audio_codec_get_volume=get_volume,.audio_hal_lock=NULL,.handle=NULL};

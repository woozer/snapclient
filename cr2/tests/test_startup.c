/* Compile the exact generated startup block against simulated queue/clock/I2S.
 * This tests cursor ownership and call ordering, not ESP scheduling or LRCLK. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cr2_startup_policy.h"
#include "cr2_dma_geometry.h"
typedef uint32_t TickType_t;
typedef int esp_err_t;
#define ESP_OK 0
#define pdTRUE 1
#define configTICK_RATE_HZ 1000
#define pdMS_TO_TICKS(x) (x)
#define USE_SAMPLE_INSERTION 1
#define ESP_ERROR_CHECK(x) assert((x)==0)
#define ESP_LOGI(...) ((void)0)
#define MEDIANFILTER_Init(x) ((void)(x))
typedef struct Fragment {char *payload;size_t size;struct Fragment *nextFragment;} Fragment;
typedef struct Chunk {struct {int64_t sec,usec;} timestamp;Fragment *fragment;} Chunk;
typedef struct {uint32_t sr,bits,ch;bool muted;} Settings;
static int64_t now_us,timer_deadline,queue_at,dispatch_late,enable_cost;
static int enabled,timer_arms,recreated,freed,abort_code,api_records,mute_calls;
static bool fail_preload,fail_enable,fail_notify;
static size_t loaded;
static uint32_t expected_word;
static Chunk *queued;
static int64_t api_deadline,api_begin,api_end;
static int64_t esp_timer_get_time(void){return now_us;}
static void audio_set_mute(bool x){(void)x;mute_calls++;}
static void my_gptimer_stop(void *p){(void)p;timer_deadline=0;}
static void my_i2s_channel_disable(void *p){(void)p;}
static void cr2_dma_probe_stop(void){}
static void audio_dac_enable(bool x){assert(x);}
static void cr2_audio_resync(int x){assert(x==1);}
static void vTaskDelay(TickType_t x){now_us+=1000*x;}
static void cr2_trace(int kind,int64_t a,int64_t b,int64_t c,int64_t d){
    if(kind==19){api_records++;api_deadline=a;api_begin=b;api_end=c;}
    if(kind==20)abort_code=(int)d;
}
static void tg0_timer1_start(int64_t delay){
    assert(loaded==(size_t)CR2_DMA_FRAMES*8); // Never arm while prefill is incomplete.
    assert(delay>0);timer_arms++;timer_deadline=now_us+delay;
}
static int xTaskNotifyWait(uint32_t a,uint32_t b,uint32_t *v,TickType_t ticks){
    (void)a;(void)b;*v=0;if(!ticks)return 0;
    assert(timer_deadline);now_us=timer_deadline+dispatch_late;
    return !fail_notify;
}
static int xQueueReceive(void *q,Chunk **c,TickType_t ticks){
    assert(q && !timer_deadline); // Queue wait must precede timer arming.
    if(!queued || queue_at>now_us+(int64_t)ticks*1000){now_us+=(int64_t)ticks*1000;return 0;}
    if(queue_at>now_us)now_us=queue_at;
    *c=queued;queued=NULL;return pdTRUE;
}
static esp_err_t cr2_i2s_preload(void *p,char *buf,size_t n,size_t *out){
    (void)p;assert(!timer_deadline && n%4==0);*out=fail_preload ? n-4:n;
    for(size_t i=0;i<*out;i+=4){uint32_t word;memcpy(&word,buf+i,4);assert(word==expected_word++);}
    loaded+=*out;now_us+=10;return ESP_OK;
}
static int my_i2s_channel_enable(void *p){(void)p;enabled++;now_us+=enable_cost;return fail_enable ? -1:0;}
static int player_setup_i2s(Settings *s){assert(s);recreated++;loaded=0;return 0;}
static void free_pcm_chunk(Chunk *c){
    assert(c);Fragment *f=c->fragment;
    while(f){Fragment *next=f->nextFragment;free(f->payload);free(f);f=next;}
    free(c);freed++;
}
static Chunk *chunk(int64_t pts,unsigned frames,unsigned start,unsigned fragment_frames){
    Chunk *c=calloc(1,sizeof(*c));c->timestamp.sec=pts/1000000;c->timestamp.usec=pts%1000000;
    Fragment **next=&c->fragment;
    while(frames){unsigned take=frames<fragment_frames ? frames:fragment_frames;
        Fragment *f=calloc(1,sizeof(*f));*next=f;f->size=take*4;f->payload=malloc(f->size);
        for(unsigned i=0;i<take;i++){uint32_t word=start++;memcpy(f->payload+i*4,&word,4);}
        next=&f->nextFragment;frames-=take;
    }return c;
}
typedef struct {int sync;size_t remaining;uint64_t samples;int64_t cursor,start;uint32_t next_word;} Result;
static Result run(Chunk *chnk,int64_t first_pts,int64_t buffer_us){
    Settings scSet={48000,16,2,false};
    int i2sDmaBufCnt=2,i2sDmaBufMaxLen=CR2_DMA_FRAMES;
    int shortMedianFilter=0,miniMedianFilter=0,initialSync=0,dir=0,dir_insert_sample=0;
    int64_t insertedSamplesCounter=0,chunkStart=first_pts,buf_us=buffer_us,clientDacLatency_us=0,diff2Server=0,playback_start_time_us=0,age=0;
    uint64_t samples_written=0;size_t alreadyWritten=0,size=0,written=0;
    void *gptimer=NULL,*tx_chan=NULL,*pcmChkQHdl=(void*)1;uint32_t notifiedValue=0;
    Fragment *fragment=NULL;char *p_payload=NULL;
    for(int iteration=0;iteration<1;iteration++) {
#include "startup_block.c.inc"
    }
    uint32_t next_word=UINT32_MAX;if(size)memcpy(&next_word,p_payload,4);
    Result r={initialSync,size,samples_written,chunkStart,playback_start_time_us,next_word};
    if(chnk)free_pcm_chunk(chnk);
    // Refer to state reset by the real block; assert cleanup, not just outcomes.
    assert(!dir && !dir_insert_sample && !insertedSamplesCounter && !alreadyWritten);
    return r;
}
static void reset(void){
    if(queued)free_pcm_chunk(queued);queued=NULL;
    now_us=30000;timer_deadline=0;queue_at=40000;dispatch_late=50;enable_cost=10;
    enabled=timer_arms=recreated=freed=abort_code=api_records=mute_calls=0;
    loaded=expected_word=0;fail_preload=fail_enable=fail_notify=false;
}
static Result standard(int64_t buffer){
    queued=chunk(24000,1152,1152,200);return run(chunk(0,1152,0,200),0,buffer);
}
int main(int argc,char **argv){
    reset();Result r=standard(100000);
    assert(r.sync && enabled==1 && !recreated && r.samples==2*CR2_DMA_FRAMES);
    assert(r.cursor==(int64_t)2*CR2_DMA_FRAMES*1000000/48000);
    assert(r.next_word==2*CR2_DMA_FRAMES);assert(api_records==1 && api_deadline==100000 && api_begin==100050 && api_end==100060);
    assert(r.start==100060); // Actual return time; independent of the 2ms mute delay.
    // DMA ending exactly at the chunk boundary must not reread or wait for another chunk.
    reset();r=run(chunk(0,2*CR2_DMA_FRAMES,0,CR2_DMA_FRAMES),0,100000);
    assert(r.sync && !r.remaining && freed==1 && enabled==1);
    reset();now_us=100001;r=standard(100000);
    assert(!r.sync && !enabled && !timer_arms && recreated==1 && abort_code==1);
    reset();queue_at=65500;r=standard(50000);
    if(CR2_DMA_FRAMES==1023)assert(!r.sync && !enabled && !timer_arms && recreated==1 && abort_code==2);
    else assert(r.sync && enabled==1 && queued); // First FLAC chunk suffices.
    reset();fail_preload=true;r=standard(100000);
    assert(!r.sync && !enabled && !timer_arms && recreated==1 && abort_code==5);
    reset();dispatch_late=2500;r=standard(100000);
    assert(!r.sync && !enabled && recreated==1 && abort_code==7);
    reset();dispatch_late=-1;r=standard(100000);
    assert(!r.sync && !enabled && recreated==1 && abort_code==7);
    reset();fail_notify=true;r=standard(100000);
    assert(!r.sync && !enabled && recreated==1 && abort_code==6);
    reset();enable_cost=2500;r=standard(100000);
    assert(!r.sync && enabled==1 && recreated==1 && api_records==1 && abort_code==8);
    reset();fail_enable=true;r=standard(100000);
    assert(!r.sync && enabled==1 && recreated==1 && abort_code==8);
    // A timestamp gap may not be stitched into a continuous DMA preload.
    reset();queued=chunk(25000,1152,1152,200);r=run(chunk(0,240,0,200),0,100000);
    assert(!r.sync && !enabled && recreated==1 && abort_code==3);
    reset();assert(cr2_startup_queue_ticks(10000,9000,1000)==0);
    assert(cr2_startup_queue_ticks(10000,0,100)==0);
    assert(cr2_startup_queue_ticks(10000,0,1000)==9);
    printf("PASS startup actual block: %d frames, cursor/fragments/deadlines/failures/API timing\n",CR2_DMA_FRAMES);
    if(argc==2){
        FILE *input=fopen(argv[1],"r");assert(input);
        long long first,second;unsigned cases=0,pass50=0,pass100=0;
        while(fscanf(input,"%lld %lld",&first,&second)==2){
            reset();now_us=first;queue_at=second;r=standard(50000);pass50+=r.sync;
            if(CR2_DMA_FRAMES==1023)assert(!r.sync && !enabled);
            reset();now_us=first;queue_at=second;r=standard(100000);pass100+=r.sync;
            cases++;
        }
        fclose(input);reset();assert(cases==98);
        printf("Trace-arrival replay (simulated scheduling): %u cases; 50ms starts=%u; 100ms starts=%u\n",cases,pass50,pass100);
    }
}

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "cr2_wait_policy.h"
#define configTICK_RATE_HZ 1000
#define pdPASS 1
typedef unsigned TickType_t;
typedef int BaseType_t;
typedef int pcm_chunk_message_t;
static int pcmChkQHdl,shortMedianFilter;
static int64_t now_us,arrival,deadline,invalid_at,shortMedian;
static bool no_progress,oversleep;
static unsigned peeks,trace_calls,trace_timeouts,trace_reason;
static const int hardResyncThreshold=2000;
static int64_t esp_timer_get_time(void){return now_us;}
static bool MEDIANFILTER_isFull(int *unused,int index){(void)unused;(void)index;return true;}
static int uxQueueMessagesWaiting(int q){(void)q;return now_us>=arrival;}
static uint32_t cr2_dma_probe_wait_budget(bool error,int64_t *remaining){
    *remaining=deadline-now_us+5000;
    if(error||now_us>=invalid_at||deadline<=now_us)return 0;
    return (uint32_t)(deadline-now_us);
}
static BaseType_t xQueuePeek(int q,pcm_chunk_message_t **p,TickType_t ticks){
    (void)q;(void)p;++peeks;assert(ticks>0);
    int64_t wake=(now_us/1000+ticks)*1000;
    if(oversleep)wake=deadline+1;
    if(no_progress)return 0;
    now_us=arrival<wake?arrival:wake;
    if(oversleep)now_us=wake;
    return now_us>=arrival;
}
static void cr2_trace(int kind,int64_t a,int64_t b,int64_t c,int64_t d){
    (void)c;if(kind==21){trace_calls=a;trace_timeouts=b;trace_reason=d;}
}
static int run(int64_t packet,int64_t invalid,int64_t error,bool stall,bool late){
    now_us=900;deadline=5700;arrival=packet;invalid_at=invalid;
    shortMedian=error;no_progress=stall;oversleep=late;
    peeks=trace_calls=trace_timeouts=trace_reason=0;
    int msgWaiting=0;
    #include "wait_block.c.inc"
    assert(trace_calls<=42);
    return msgWaiting;
}
int main(void){
    assert(run(4500,100000,0,false,false)==1);assert(peeks==2&&trace_timeouts==1&&trace_reason==1);
    assert(run(2500,100000,0,false,false)==1);assert(peeks==1);
    assert(run(8000,100000,0,false,false)==0);assert(now_us==5000&&peeks==2);
    assert(run(4500,3900,0,false,false)==0);assert(peeks==1&&trace_reason==3);
    assert(run(2500,100000,3000,false,false)==0);assert(peeks==0);
    assert(run(2500,100000,0,true,false)==0);assert(peeks==1&&trace_reason==4);
    assert(run(4500,100000,0,false,true)==0);assert(now_us>deadline);
    puts("PASS actual retry block: early-timeout recovery, normal arrival, deadline, invalid DMA, clock error, no progress, late wake");
}

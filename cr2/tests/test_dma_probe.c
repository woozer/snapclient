#include "cr2_dma_probe.c"
#include <assert.h>
bool cr2_sync_can_configure(void) {return true;}
static void write_bytes(unsigned bytes,bool preload){
    while(bytes){unsigned n=bytes>960?960:bytes;cr2_dma_probe_transfer_begin(n,preload);cr2_dma_probe_transfer_end(n,preload);bytes-=n;}
}
int main(void){
    const unsigned block=CR2_DMA_FRAMES*4,capacity=block*2;
    const unsigned period=cr2_dma_period_us(block,192000);
    cr2_dma_probe_attach((void*)1,48000,16,2);
    assert(state.block==block && state.capacity==capacity && !state.flags);
    fake_us=1000;write_bytes(capacity,true);cr2_dma_probe_start();
    i2s_event_data_t event={.size=block,.dma_buf=(void*)1};
    fake_us=1000+period;sent(NULL,&event,NULL);write_bytes(block,false);
    event.dma_buf=(void*)2;fake_us=1000+2*period;sent(NULL,&event,NULL);
    write_bytes(block/2/4*4,false);fake_us+=1000;
    assert(!trace_count); // No ISR/transfer logging to PSRAM.
    cr2_dma_probe_snapshot(0);assert(trace_count==4 && !last.flags);
    assert(last.lower_us==(int64_t)(block/2/4*4)*1000000/192000);
    assert(last.estimate_us==(int64_t)(block+block/2/4*4)*1000000/192000-1000);
    overflow(NULL,&event,NULL);cr2_dma_probe_snapshot(0);
    assert(last.flags&CR2_DMA_Q_OVERFLOW);assert(last.estimate_us==-1);
    cr2_dma_probe_stop();cr2_dma_probe_start();cr2_dma_probe_snapshot(0);
    assert(last.flags&CR2_DMA_NO_PREFILL);
    cr2_dma_probe_stop();write_bytes(capacity,true);fake_us=100000;cr2_dma_probe_start();
    event.dma_buf=(void*)1;fake_us=100000+period;sent(NULL,&event,NULL);
    write_bytes(block-960,false);cr2_dma_probe_transfer_begin(960,false);
    event.dma_buf=(void*)2;fake_us=100000+2*period;sent(NULL,&event,NULL);
    cr2_dma_probe_transfer_end(960,false);cr2_dma_probe_snapshot(0);
    assert(!last.flags && last.lower_us==0);
    event.dma_buf=(void*)1;fake_us=100000+3*period;sent(NULL,&event,NULL);
    cr2_dma_probe_snapshot(0);assert(last.flags&CR2_DMA_PARTIAL_ACTIVE);
    cr2_dma_probe_stop();write_bytes(capacity,true);fake_us=200000;cr2_dma_probe_start();
    event.dma_buf=(void*)1;fake_us+=isr_gap_limit_us+1;sent(NULL,&event,NULL);
    cr2_dma_probe_snapshot(0);assert(last.flags&CR2_DMA_EOF_GAP);
    cr2_dma_probe_stop();sent(NULL,&event,NULL);assert(!state.eof_count && !state.running);
    // Boundaries must scale to each descriptor size, not the old 21.313ms.
    cr2_wait_config_t cfg={true,20000,5000};
    cr2_dma_inventory_t inv={.estimate_us=10000,.since_eof_us=period};
    assert(cr2_wait_budget(inv,period+1000,false,cfg,period,1000)==5000);
    assert(!cr2_wait_budget(inv,period+1001,false,cfg,period,1000));
    inv.since_eof_us=period+1;assert(!cr2_wait_budget(inv,period,false,cfg,period,1000));
    inv.since_eof_us=0;assert(!cr2_wait_budget(inv,period,true,cfg,period,1000));
    inv.flags=CR2_DMA_PARTIAL_ACTIVE;assert(!cr2_wait_budget(inv,period,false,cfg,period,1000));
    inv.flags=0;inv.estimate_us=5000;assert(!cr2_wait_budget(inv,period,false,cfg,period,1000));
    assert(cr2_wait_accept(5000,4999,true,1));assert(!cr2_wait_accept(5000,5001,true,1));
    printf("PASS probe/wait: 2x%d, period %u us, ISR limit %u us\n",CR2_DMA_FRAMES,period,isr_gap_limit_us);
}

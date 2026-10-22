          const uint32_t frame_bytes=scSet.ch*(scSet.bits/8);
          const size_t capacity=(size_t)i2sDmaBufCnt*i2sDmaBufMaxLen*frame_bytes;
          const int64_t first_pts=chunkStart;
          const int64_t deadline=first_pts+buf_us-clientDacLatency_us-diff2Server;
          size_t prefilled=0;
          int abort_reason=(scSet.sr==48000 && scSet.bits==16 && scSet.ch==2) ? 0 : 9;
          MEDIANFILTER_Init(&shortMedianFilter);
          MEDIANFILTER_Init(&miniMedianFilter);
          audio_set_mute(true);
          my_gptimer_stop(gptimer);
          my_i2s_channel_disable(tx_chan);
          cr2_dma_probe_stop();
          while(xTaskNotifyWait(0,UINT32_MAX,&notifiedValue,0)==pdTRUE) {}
          cr2_trace(17,first_pts,deadline,esp_timer_get_time(),i2sDmaBufMaxLen);
#if !USE_SAMPLE_INSERTION
          adjust_apll(0);
#endif
          fragment=chnk->fragment;
          p_payload=fragment ? fragment->payload : NULL;
          size=fragment ? fragment->size : 0;
          while(!abort_reason && prefilled<capacity) {
            if(!cr2_startup_can_arm(deadline,esp_timer_get_time())) {
              abort_reason=1; break; /* Prefill missed original deadline. */
            }
            if(chnk==NULL) {
              TickType_t ticks=cr2_startup_queue_ticks(deadline,esp_timer_get_time(),configTICK_RATE_HZ);
              if(!ticks || !pcmChkQHdl ||
                 xQueueReceive(pcmChkQHdl,&chnk,ticks)!=pdTRUE) {
                abort_reason=2; break;
              }
              int64_t next_pts=(int64_t)chnk->timestamp.sec*1000000+chnk->timestamp.usec;
              int64_t expected=cr2_startup_cursor_pts(first_pts,prefilled/frame_bytes,scSet.sr);
              if(next_pts<expected-2 || next_pts>expected+2) {
                abort_reason=3; break; /* Never concatenate discontinuous PTS. */
              }
              fragment=chnk->fragment;
              p_payload=fragment ? fragment->payload : NULL;
              size=fragment ? fragment->size : 0;
            }
            if(!fragment || !p_payload || !size || size%frame_bytes) {
              abort_reason=4; break;
            }
            size_t take=size<capacity-prefilled ? size : capacity-prefilled;
            esp_err_t load=cr2_i2s_preload(tx_chan,p_payload,take,&written);
            if(load!=ESP_OK || written!=take) {abort_reason=5;break;}
            prefilled+=written;size-=written;p_payload+=written;
            if(!size) {
              if(fragment->nextFragment) {
                fragment=fragment->nextFragment;
                p_payload=fragment->payload;size=fragment->size;
              } else {
                free_pcm_chunk(chnk);chnk=NULL;fragment=NULL;p_payload=NULL;
              }
            }
          }
          /* The cursor is the first UNSENT sample, not first_pts minus DMA.
           * Handles partial chunks, fragment boundaries and exact DMA fills. */
          if(!abort_reason)
            chunkStart=cr2_startup_cursor_pts(first_pts,prefilled/frame_bytes,scSet.sr);
          int64_t ready_us=esp_timer_get_time();
          cr2_trace(18,deadline,ready_us,prefilled,abort_reason);
          if(!abort_reason && !cr2_startup_can_arm(deadline,ready_us)) abort_reason=1;
          if(!abort_reason) {
            audio_dac_enable(true);
            int64_t arm_us=esp_timer_get_time();
            if(!cr2_startup_can_arm(deadline,arm_us)) abort_reason=1;
            else {
              tg0_timer1_start(deadline-arm_us);
              TickType_t timeout=pdMS_TO_TICKS((deadline-arm_us+999)/1000+5)+1;
              if(xTaskNotifyWait(0,UINT32_MAX,&notifiedValue,timeout)!=pdTRUE)
                abort_reason=6;
              my_gptimer_stop(gptimer);
            }
          }
          if(!abort_reason) {
            int64_t begin_us=esp_timer_get_time();
            if(!cr2_startup_can_enable(deadline,begin_us)) abort_reason=7;
            else {
              esp_err_t enabled=my_i2s_channel_enable(tx_chan);
              int64_t end_us=esp_timer_get_time();
              cr2_trace(19,deadline,begin_us,end_us,enabled);
              if(enabled!=ESP_OK || !cr2_startup_can_enable(deadline,end_us)) abort_reason=8;
              else {
                playback_start_time_us=end_us;
                samples_written=prefilled/frame_bytes;
                alreadyWritten=0;
                age=end_us-deadline;
              }
            }
          }
          if(abort_reason) {
            cr2_trace(20,deadline,esp_timer_get_time(),prefilled,abort_reason);
            cr2_audio_resync(1);
            audio_set_mute(true);
            my_gptimer_stop(gptimer);
            my_i2s_channel_disable(tx_chan);
            cr2_dma_probe_stop();
            if(chnk) free_pcm_chunk(chnk);
            chnk=NULL;fragment=NULL;p_payload=NULL;size=0;
            samples_written=0;playback_start_time_us=0;alreadyWritten=0;
            initialSync=0;dir=0;dir_insert_sample=0;insertedSamplesCounter=0;
            /* IDF has no public rewind for a partial preload while READY.
             * Recreate the stopped channel; never clock stale PCM to clear it. */
            ESP_ERROR_CHECK(player_setup_i2s(&scSet));
            continue;
          }
          initialSync=1;
          vTaskDelay(pdMS_TO_TICKS(2));
          audio_set_mute(scSet.muted);
          cr2_trace(5,age,diff2Server,first_pts,0);
          ESP_LOGI(TAG,"initial I2S enable lateness: %lldus, prefilled: %u bytes",age,(unsigned)prefilled);
          if(size==0) continue;

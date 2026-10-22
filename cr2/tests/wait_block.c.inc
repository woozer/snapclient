          /* One absolute budget for this empty-queue episode. A FreeRTOS
           * timeout can return early because it counts tick boundaries.
           * Revalidate DMA before every retry; never extend the first budget.
           */
          if (msgWaiting == 0) {
            bool clock_error = MEDIANFILTER_isFull(&shortMedianFilter, 0) &&
                ((shortMedian > hardResyncThreshold) ||
                 (shortMedian < -hardResyncThreshold));
            int64_t wait_started = esp_timer_get_time();
            int64_t remaining_us = -1;
            uint32_t budget_us = cr2_dma_probe_wait_budget(clock_error, &remaining_us);
            TickType_t first_ticks = (uint64_t)budget_us * configTICK_RATE_HZ / 1000000;
            if (first_ticks) {
              const int64_t deadline_us = wait_started + budget_us;
              int64_t remaining_after = remaining_us;
              uint32_t after_budget = budget_us;
              unsigned calls = 0, timeouts = 0, reason = 2;
              cr2_trace(15, remaining_us, budget_us, wait_started, first_ticks);
              /* The cap is at most40ms. Each blocked call must advance time;
               * a fixed bound also protects against an unexpected API behavior.
               */
              while (calls < 42) {
                int64_t now = esp_timer_get_time();
                after_budget = cr2_dma_probe_wait_budget(clock_error, &remaining_after);
                if (!after_budget) { reason = 3; break; }
                int64_t left = deadline_us - now;
                if (left <= 0) break;
                if (left > after_budget) left = after_budget;
                TickType_t ticks = (uint64_t)left * configTICK_RATE_HZ / 1000000;
                /* Do not round up into the reserve or spin for a sub-tick tail. */
                if (!ticks) break;
                pcm_chunk_message_t *pending = NULL;
                BaseType_t ready = xQueuePeek(pcmChkQHdl, &pending, ticks);
                ++calls;
                if (ready != pdPASS) ++timeouts;
                after_budget = cr2_dma_probe_wait_budget(clock_error, &remaining_after);
                int64_t returned = esp_timer_get_time();
                if (cr2_wait_accept(budget_us, returned - wait_started,
                                    ready == pdPASS, after_budget)) {
                  msgWaiting = uxQueueMessagesWaiting(pcmChkQHdl);
                  if (msgWaiting) { reason = 1; break; }
                }
                if (!after_budget) { reason = 3; break; }
                if (returned <= now) { reason = 4; break; }
                /* Timeout: next iteration uses fresh inventory and the same
                 * deadline, rather than immediately forcing a hard resync. */
              }
              cr2_trace(16, esp_timer_get_time() - wait_started,
                        remaining_after, after_budget, msgWaiting);
              /*21: peek calls, timeouts, absolute deadline, exit reason:
               *1queued,2deadline/sub-tick/call bound,3invalid budget,4no progress. */
              cr2_trace(21, calls, timeouts, deadline_us, reason);
            }
          }

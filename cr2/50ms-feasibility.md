# 50 ms feasibility review — 22 October 2026

**50 ms remains an experimental target, not a reliable operating setting.**
The startup correction addresses a demonstrated bug. Retrying an early queue
timeout recovers some FLAC arrivals, but has not improved the complete test.
PCM is substantially more promising than this FLAC packetization. Delivery
outliers and DMA accounting/feeding still need separate investigation.

## What the latest retry experiment established

Same live 200 Hz source, protective CamillaDSP filters / 512 frames, 48 kHz
stereo S16, wired Linux server, one AP, RSSI −46 to −45 dBm. Amplifier and
charging were off. These are transport tests, not listening tests.

| Firmware / transport | Target | Measured interval | Resyncs |
|---|---:|---:|---:|
| Startup fix, DMA 2×240, FLAC | 50 ms | 60.157 s | 704 |
| Startup fix, DMA 2×240, verified PCM 10 ms, reserve 3 ms | 50 ms | 60.191 s | 9 |
| Startup fix + repeated wait, DMA 2×240, FLAC | 50 ms | 60.283 s | 765 |
| Startup fix + repeated wait, DMA 2×240, verified PCM 10 ms, reserve 3 ms | 50 ms | 60.204 s | 13 |
| Startup fix + repeated wait, DMA 2×240, FLAC control | 100 ms | 20.362 s | 0 |

These sequential, short trials do not establish that the retry caused the
higher counts. They do establish that it did not deliver a successful 50 ms
run. Do not promote this experiment over the existing 100 ms reference.

## Code review

1. **Startup deadline: useful correction.** Prefill happens before arming the
   original PTS deadline. The cursor advances only for submitted samples.
   Late/failed startup is rejected, stale partial preload is cleared by
   recreating the READY I2S channel, and actual enable API entry/return are
   traced. The retry trials logged API return 65–109 µs after the deadline
   (FLAC) and 66–84 µs (PCM), across the captured startups. That is not a
   measurement of LRCLK, analogue output, speaker sound or total lip-sync.
   The 1 ms arm margin and 2 ms dispatch allowance remain engineering policies.
2. **Repeated wait: bounded, but limited benefit.** Each attempt uses fresh DMA
   accounting and the original absolute deadline; it cannot extend into the
   reserve. It neither consumes a queued chunk nor alters its timestamp. The
   host tests exercise timeout, invalid inventory and late wake cases. Of 930
   FLAC wait episodes, 473 retried and 25 recovered only on the second attempt.
   PCM had three retries and none recovered. This is not a missing large buffer.
3. **Reserve and descriptor geometry matter.** Two 240-frame descriptors hold
   only 10 ms at 48 kHz. A 5 ms reserve leaves little waiting room, especially
   when the player checks the software queue near a descriptor boundary.
   Tick-flooring can leave a sub-millisecond tail; the code deliberately does
   not spin or round up into the reserve. Many 'arrival before estimated DMA
   exhaustion' cases are consequently not recoverable within its safe budget.
4. **DMA validity is still provisional.** Three PCM playback resyncs had valid
   inventory (~3.1–3.3 ms remaining), but their next chunks arrived ~9.3–14.1 ms
   later. The other three had partial-active flags; one also had a send-queue
   overflow. Counting accepted bytes and EOFs is useful, but does not prove
   which samples the peripheral physically emitted. Do not suppress these
   flags or treat the legacy zero-underrun counter as evidence of continuity.
5. **Clock estimates are not a hardware clock measurement.** The player still
   derives error from submitted sample counts, a startup timestamp and DMA
   geometry. The original sample corrections remain. A logic-analyser LRCLK/
   DATA measurement or recorded output comparison is needed to validate this
   model, especially through partial writes, insertions and startup recovery.
6. **Priority/radio changes are not justified by these traces alone.** Wi-Fi
   sleep was already off. Player priority 20/core 1, receiver priority 17/core 1,
   lwIP priority 18 and Wi-Fi/core 0 were unchanged. Raising everything can
   merely move starvation elsewhere; measure the task that is delayed first.

## Where the long delivery gaps occur

In the measured PCM window:

- Server Ethernet capture: chunk completion intervals median 10.003 ms,
  p99 10.281 ms, maximum 10.472 ms.
- ESP receive-callback gap: maximum **57.517 ms**.
- Callback-to-receiver-return: median 0.638 ms, p99 1.279 ms, maximum 5.162 ms.
- PCM handler: median 0.024 ms, p99 0.034 ms, maximum 0.252 ms.
- Server-NIC-to-complete-body relative lag: worst **48.283 ms above its window
  median**. This aligns different clock origins by their median, without a
  drift fit; it is not an absolute one-way latency measurement.
- No repeated TCP sequence ranges or zero advertised windows were observed in
  the measured capture. This does **not** rule out Wi-Fi MAC retries, AP
  buffering, driver/lwIP delays or callback scheduling.

The largest gaps therefore precede delivery to the receive callback, rather
than coming mainly from the PCM handler. That narrows the search but does not
identify RF interference as the sole cause. A strong RSSI is not a latency or
channel-utilization measurement.

FLAC makes this particular 50 ms experiment harder: observed 24 ms audio
chunks, often emitted at 20/40 ms spacing. It saves bandwidth but consumes
more of the small scheduling budget before arrival. PCM 10 ms needs 1.536
Mbit/s payload; bandwidth is affordable in good conditions, but worst-case
arrival time remains decisive.

## Next experiments and decision criteria

- **Completed:** verified PCM 10 ms with DMA 2×480 (20 ms capacity), reserve
  3 ms. The first control/trial joined different APs despite the board staying
  in place: 100 ms had zero resyncs over 20.422 s at −68..−67 dBm; 50 ms had
  218 over 60.180 s at −72..−70 dBm. This is not a controlled DMA comparison.
- **Repeated with explicit strongest-AP selection:** the same AP remained
  connected throughout both runs. At −43 dBm, the 100 ms PCM control had zero
  resyncs over 20.362 s. At −44..−43 dBm, 50 ms had **five over 60.216 s**:
  three playback resyncs with partial-active DMA flags and two late chunks.
  No startup deadline aborts occurred; I2S enable API returned 69–88 µs late.
  All source/capture/trace checks passed. These silent trials still fail the
  zero-resync criterion; they do not isolate DMA size from changing RF conditions.
- In that stronger-AP 50 ms window, server sends stayed within 10.613 ms,
  but receive notifications had gaps up to 49.577 ms. Callback-to-receive-return
  was median 0.639 ms / max 4.787 ms; handler time max 0.355 ms. Relative
  delivery lag reached 40.594 ms above its median. No repeated TCP ranges or
  zero windows were seen. A notification gap is not a callback sleeping: the
  missing delivery precedes the callback, whose hook runs after lwIP enqueues
  received data. AP/radio/driver/lwIP scheduling remain unresolved contributors.
- Compare the same firmware/source with a dedicated AP radio, fixed 20 MHz
  channel and wired server. A separate SSID on the same loaded radio does not
  reserve airtime. Measure repeated runs and tail gaps, not just average ping.
- Only after clean transport, perform an audible test and electrical/acoustic
  timing measurement with the protective crossover retained. Source/DSP delay
  before Snapserver and amplifier delay are additional to the configured PTS
  target; 50 ms is not yet measured end-to-end latency.
- For confidence, require repeated multi-minute zero-resync runs, verified DMA
  continuity and later realistic network load/multiple speakers. A clean
  one-minute run is encouraging, not sufficient for a surround specification.

## Dedicated wireless link / ESP-NOW

The current ESP32-S3 is 2.4 GHz 802.11b/g/n, with a single transmit/receive radio
chain. It can join a dedicated AP or host SoftAP; it cannot receive 5 GHz or
create a steerable beam through firmware. A USB adapter at the server does not
change the receiver's supported band. A dedicated AP is the least disruptive
comparison because it retains Snapcast and our existing synchronization.

ESP-NOW bypasses association-based IP/TCP transport but still uses Wi-Fi radio
frames in the same shared band. It would require a sender bridge and new audio
framing, sequence/loss handling and clock synchronization. Its default 1 Mbit/s
PHY rate is below our 1.536 Mbit/s PCM payload requirement; configurable higher
rates do not themselves guarantee delivery deadlines. It is an alternative
transport project, not a demonstrated fix for these failures.

Official references: [ESP32-S3 datasheet](https://documentation.espressif.com/esp32_s3_datasheet_en.pdf),
[ESP-IDF 5.5.1 Wi-Fi](https://docs.espressif.com/projects/esp-idf/en/v5.5.1/esp32s3/api-guides/wifi.html),
[ESP-NOW](https://docs.espressif.com/projects/esp-idf/en/v5.5.1/esp32s3/api-reference/network/esp_now.html),
[I2S APIs](https://docs.espressif.com/projects/esp-idf/en/v5.5.1/esp32s3/api-reference/peripherals/i2s.html).

## Evidence and privacy

[Sanitized result summary](results/wait-retry-20261022.json). Raw captures
remain local and are not bundled here. Firmware candidate: `0.2.26-retry`;
100 ms reference: `0.2.20-wait`. The original reference was restored and checked
healthy after these trials, with amp/charge off. A subsequent experiment has
its own result; this document does not claim a live runtime state.

The later same-AP results are in
[the AP/DMA comparison](results/best-ap-50ms-20261022.json). Firmware
`0.2.29-ap480` was tested, then `0.2.28-ap100` restored and verified. The latter
preserves the reference `0.2.20-wait` audio code and DMA 2×1023, changing only
the version and explicit all-channel / strongest-signal AP selection. It scans
at connection time, clears an old BSSID/channel hint, retains credentials and
authentication policy, and does not continuously scan during playback.
Production remains 100 ms / FLAC / Camilla512, wait20/reserve5, amp and charge
off. Exact AP IDs remain in local operational notes, not the public summary.

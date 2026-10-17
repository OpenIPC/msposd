# AHI Refresh Jitter Analysis

As of 2026-10-09.

## Summary

The AHI ladder on the ground station flickers and trails because attitude samples reach the screen at irregular moments, not because of how the ladder is drawn. Each presented frame contains exactly one clean ladder; the hold time of a frame varies from 16 ms to over 100 ms, and the eye reads that as a flicker plus a shadow of the previous position.

Measured end to end on 2026-10-09 with the air unit (OpenIPC SSC30KQ, INAV over 115200 baud) and the ground station recorded at the same time: the wifibroadcast link added almost nothing (90% of packets within 6 ms of the minimum). The irregularity comes from the air unit side in two parts. About 12 to 17% of attitude requests get no reply from the FC at all, which is a 66 ms hold each time at the 30 Hz poll rate. A further 5% of replies arrive one or two ticks late because INAV's once-per-second full OSD redraw, about 1 KB, occupies the serial line for roughly 90 ms.

A ground-side fixed-cadence display slot with a 20 ms margin, as proposed, would still see about one slot in four without a fresh sample on today's stream. Fixing the source first (missing replies, serial saturation) is what makes a small margin realistic.

## Update 2026-10-18: INAV request spacing and the new poll schedule

A direct test against the INAV FC found a rule the analysis below did not know about: **INAV answers only the first of two MSP requests that arrive less than about 10 ms apart.** The second gets an MSP error frame (`$M!`), which msposd's parser drops without a trace. A scratch tool on the camera sent request pairs at 30 Hz:

| Gap between the two requests | Second request answered |
| --- | --- |
| 0 ms (same write) | 5% |
| 2 ms | 22% |
| 5 ms | 55% |
| 8 ms | 84% |
| 10 ms or more | 100% |

Any request sent alone got 100%, and the same command sent twice failed the same way, so the rule is not command specific. About 10 ms matches INAV's 100 Hz serial task.

**Effect on the old schedule.** The build measured below wrote attitude and then up to three companion requests (altitude, RC, GPS, status) in the same tick. Attitude was answered, but the companions mostly were not: RC about 8%, altitude about 33%, GPS and status 5 to 10%. That is what broke RC channel monitoring.

**New schedule in `send_variant_request2` (`msposd.c`).** Each tick sends attitude first, then at most one other request, picked by a 5-tick cycle: a slow slot (FC variant, status, VTX config, UID, once per second each), then RC, altitude, raw GPS, comp GPS. With the AHI on, that request is sent by the early-read timer 10 ms after attitude, and the early read moved from 15 ms to 10 ms with it. The simple UART poll rate is now capped at 40 Hz.

Measured on the camera with AHI 4 and RC monitoring:

| Poll rate | Attitude answered | Other requests answered | Average attitude delay (msposd stat) |
| --- | --- | --- | --- |
| 20 Hz | 100% | 100% | 16 ms |
| 30 Hz | 99.9% | 100% | 14 ms |
| 40 Hz | 95.4% | about 99% | 13 ms |

At 40 Hz a tick is 25 ms. When the busy camera fires the mid-tick timer more than about 5 ms late, the gap to the next attitude request falls under 10 ms and INAV rejects it. With the AHI on, 30 Hz is the practical maximum.

**What this does and does not explain.** The 12 to 17% unanswered attitude requests below were measured while attitude was already first in the tick, so the rule does not explain them directly. The same build answered 100% of attitude requests in a capture on 2026-10-11. One candidate, not verified: under load, a tick that fires more than about 23 ms late is followed almost at once by the next scheduled tick, which puts two attitude requests less than 10 ms apart. The 95th-percentile send delay of 35 ms in the table below shows such late ticks do happen. Re-measure with the relay recipe before acting on root cause 1.

## Symptom

While the aircraft rolls or pitches, the ladder appears to flicker and a faint copy of the line seems to linger at its previous position. The effect is strongest on the thin 1 px sublines and on the 3 px main line during continuous motion.

The rendered output is clean. A 60 fps screen recording of the OSD window, inspected frame by frame, shows a single ladder per frame with no residue; the double buffer is fully rewritten every frame. The trail is a perception effect: the eye tracks the expected motion while the display holds an old position for a variable number of refreshes, then jumps. The longer and more uneven the holds, the more visible the shadow.

A second, smaller contributor was in the drawing: rotated line endpoints were truncated to integers, so the short sublines wobbled by up to a pixel while rolled. That is fixed (double-precision rotation in `drawLineGS`, `osd/util/Render_gs.c`) and is not part of the timing problem below.

## How the AHI is drawn today

The ladder is driven by a dedicated MSP_ATTITUDE poll, separate from the OSD DisplayPort stream, so that the GS shows attitude with the lowest possible latency. The chain below is the one in use on the test setup.

1. **Air unit tick.** msposd on the camera runs with `-r 1030`: simple UART mode, 30 polls per second (33.3 ms tick). Each tick it reads the serial port, then writes an MSP_ATTITUDE request, and arms a one-shot early read 15 ms later (`send_variant_request2`, `early_read` in `msposd.c`).
2. **Reply pickup.** The reply is picked up at that early read if it has fully arrived, otherwise at the next tick. So a reply leaves the camera at one of two moments, about 18 ms apart, plus timer latency on the busy SoC (load average around 14).
3. **Forwarding.** The attitude message is sent immediately as its own UDP datagram to 127.0.0.1:14550 (MSP_ATTITUDE case in `osd.c`), where wfb-ng's telemetry_tx carries it over the air. OSD DisplayPort data is aggregated and sent only on DRAW_SCREEN, at most every 66 ms.
4. **Ground station.** msposd on the GS redraws the whole OSD synchronously inside the UDP receive path on every attitude message (`force_draw_ahi` → `draw_screenBMP2`), with no throttle (`-r 999`). DRAW_SCREEN messages trigger further full redraws, about 17 per second, which do not move the ladder. Each redraw costs 7 to 10 ms.
5. **Presentation.** The frame is pushed with XShmPutImage into a double-buffered ARGB window (`FlushDrawing` in `osd/util/Render_gs.c`) and composited by xfwm4 at the 60 Hz refresh. A frame is therefore shown for a whole number of refreshes.

There is no timestamp in the attitude message. The GS draws each sample at its arrival time and cannot tell a late packet from a late FC reply.

## Measurements

Three captures were taken on the bench on 2026-10-08 and 2026-10-09 with the FC connected and the aircraft handled by hand. All timing is relative to the camera's 33.3 ms poll tick, reconstructed from the data; lost samples are counted separately from late ones.

**Method.** On the GS, a 60 fps screen recording gave the presentation timing. On the camera, a small static relay timestamped every attitude datagram and forwarded it to the normal wifibroadcast port, while the GS timestamped the same datagrams on arrival. Datagrams survive the link byte-identical, so they were paired by content hash to get per-packet link delay.

| Stage | Metric | Value |
| --- | --- | --- |
| GS screen | Distinct frames presented | about 30 per second |
| GS screen | Refreshes a frame stays on screen | 1 to 7 (16 ms to 116 ms), no stable pattern |
| Camera | Attitude replies per second at 30 requests | 24 to 29 |
| Camera | Requests with no reply | 12% to 17% of ticks |
| Camera | Reply read after request (msposd stat) | 19 to 20 ms average |
| Camera | Send delay vs tick, median / 90% / 95% / 99% | 7 / 13 / 35 / 73 ms |
| Camera | Replies delayed more than 25 ms when serial line quiet | 0% |
| Camera | Replies delayed more than 25 ms right after a full OSD frame | 28% |
| Serial | INAV full OSD redraw | once per second, 820 to 1035 bytes, 62 to 87 draw-string messages |
| Serial | Incremental OSD updates | 3.6 per second, under 100 bytes |
| Serial | UART errors | no overruns, 5 framing errors total |
| Link | Delay variation, median / 90% / 95% / 99% / max | 0 / 6 / 18 / 34 / 37 ms |
| Link | Packet loss | 0.5% |
| End to end | Delay vs tick at the GS, 90% / 99% | 27 / 85 ms |

The missing replies are spread evenly over the 30-tick poll cycle, so they are not caused by the companion requests (altitude, RC, GPS) that share certain ticks. An earlier GS-only capture on 2026-10-08 showed a much worse link (90% of samples needed 83 ms), so link conditions vary between sessions; the simultaneous capture is the authoritative one.

## Root causes, ranked

1. **Unanswered attitude requests (12 to 17%).** Each one is a 66 ms hold at 30 Hz, the single largest source of visible stutter. The requests are written, the UART shows no overruns, and the misses do not line up with any companion request, so INAV most likely does not answer them. The exact reason is not pinned down. See the 2026-10-18 update: INAV rejects a request that arrives less than 10 ms after the previous one, and late ticks bunching up on the busy camera may cause exactly that.
2. **Serial line saturation by the full OSD redraw (5% of replies, 35 to 93 ms late).** INAV redraws the whole OSD once per second as 60 to 90 short draw-string messages, about 1 KB on the wire, which takes roughly 90 ms at 115200 baud. Attitude replies queue behind it. Late replies cluster in a run of about six consecutive ticks once per second, matching this burst.
3. **Two-moment reply pickup on the camera (18 ms spread).** The early read at 15 ms catches most replies; the rest wait for the next tick. This is a bounded, predictable jitter and small compared with items 1 and 2. Judged acceptable.
4. **Sample-and-hold on a 60 Hz display.** A 30 Hz stream cannot be held for a constant number of refreshes unless its rate divides the refresh rate and its phase is locked. Even a perfect stream would alternate 33 ms and 50 ms holds against a 20 fps slot.
5. **Wifibroadcast link.** Negligible in the simultaneous capture (99% within 34 ms, 0.5% loss). It was far worse in one earlier capture, so it can dominate on a bad day, but it is not the cause of the baseline problem.

The GS render path is not a cause. Every frame is a complete redraw, no residue, and the extra DRAW_SCREEN redraws do not move the ladder. Their 7 to 10 ms cost only adds a small delay when they collide with an attitude sample.

## What a fixed-cadence display slot would need

The proposal is to draw the ladder at a fixed 20 fps (every third refresh), each slot showing the newest sample received, so that a sample a few milliseconds late still makes its slot. The scheme is sound; the question is the margin.

**Margin.** A sample misses its slot when its delay, relative to the earliest arrivals, exceeds the margin. The slot then repeats the previous position. Measured on the simultaneous capture:

| Margin | Samples that miss their slot | Slots with nothing new regardless (lost replies) |
| --- | --- | --- |
| 20 ms | 12% | 13% |
| 30 ms | 9% | 13% |
| 50 ms | 4% | 13% |
| 80 ms | 1.4% | 13% |
| 100 ms | 0.5% | 13% |

With a 20 ms margin, roughly one slot in four shows no new sample today. With the missing replies and the serial stall fixed at the source, the remaining camera jitter is the 18 ms pickup spread plus timer noise, and a 20 ms margin becomes realistic.

**Two constraints the scheme must also meet.**

- The poll rate must be an integer multiple of the slot rate. A 30 Hz stream against 20 fps slots advances by one sample, then two, which is a built-in judder even with perfect delivery. For 20 fps slots, poll at 20 or 40 Hz. With a 30 Hz poll, use every second refresh (30 fps) instead.
- The slot phase must be locked to the stream. A free-running timer sits at a random phase against the arrivals, so the effective margin is anywhere from 0 to one slot. Slots have to be scheduled as expected arrival plus margin, tracked from the earliest-arriving packets.

Polling at 40 Hz for 20 fps slots has a side benefit: when the newest sample is late, the slot can fall back to the 25 ms older one instead of repeating a 50 ms old frame. The cost is about 5% lost attitude replies at 40 Hz (see the 2026-10-18 update).

## Recommended actions, in order

1. **Raise the FC serial baud rate** to 230400 or higher on both INAV and the camera. Config only. Shortens the once-per-second OSD stall in proportion and removes most of the 35 to 93 ms tail. If INAV drops replies while its TX buffer is busy, this should also cut the missing replies; re-measure to find out.
2. **Trim the INAV OSD layout.** Each element costs about 11 bytes of protocol overhead regardless of its text, so the burst scales with the element count. Disabling INAV's own horizon and crosshair, which the msposd ladder replaces, is the obvious first cut.
3. **Re-measure the missing replies** after 1 and 2 with the relay recipe below. If 10% or more of requests still go unanswered, the cause is on the INAV side and needs investigation there, not in msposd.
4. **Match the poll rate to the display cadence** on the air unit: `-r 1020` for a 20 fps slot, or keep 30 Hz and use a 30 fps slot. `-r 1040` loses about 5% of attitude replies (see the 2026-10-18 update).
5. **Then implement the GS slot scheme** with phase lock and a margin chosen from the new measurement, expected around 20 ms once the source is clean. The double-precision line rotation already in `Render_gs.c` stays regardless.

Not recommended: buffering on the GS against today's stream. Reaching 99% of samples on time would need about 100 ms of margin, and the 13% missing replies would still repeat frames.

## Appendix: reproducing the measurements

**Ground-side arrival timing (no camera access needed).** Stop the GS msposd so port 14550 is free, then bind a raw UDP socket on 127.0.0.1:14550 and log a monotonic timestamp per datagram. Scan each datagram for MSP v1 headers (`$M`, size, cmd); cmd 108 is MSP_ATTITUDE, cmd 182 is DisplayPort. Fit the tick period and assign each sample the latest tick consistent with ordering; the residual is its delay, and ticks without a sample are lost replies.

**Camera-side timing.** The camera (busybox, no tcpdump or strace) accepts a static ARM binary built with `toolchain/sigmastar-infinity6e/bin/arm-linux-gcc -static`; copy it with `scp -O` or `cat file | ssh root@cam 'cat > /tmp/x'` since there is no sftp server. A 60-line relay binds 14551, timestamps every datagram, logs an FNV-1a hash of those holding cmd 108, and forwards everything to 127.0.0.1:14550 so the wifibroadcast path keeps working. Kill msposd on the camera, start the relay, start msposd with the production arguments but `--out 127.0.0.1:14551` and `-v`, capture on the GS at the same time, then restart msposd with the original command using `setsid nohup`.

**Pairing.** Hash the full datagram with the same constant on both ends and align the two hash sequences (a sequence matcher handles drops). Link delay per packet is the GS timestamp minus the camera timestamp, minus the minimum over the run; the clocks are not synchronised, so only the variation is meaningful.

**Presentation timing on the GS.** Record the screen with `ffmpeg -f x11grab -framerate 60` while msposd runs, crop the ladder region, count runs of identical consecutive frames. The run lengths are the hold times in refreshes.

**msposd's own stats.** With `-v`, the camera prints once per second `AttitMSGs:N(Xms)`: N replies received and the average request-to-reply time. 30 requests per second with N around 26 is the missing-reply symptom.

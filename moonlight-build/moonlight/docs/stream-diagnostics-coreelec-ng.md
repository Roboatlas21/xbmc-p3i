# Streaming health logging on CoreELEC NG

Luna's Advanced settings → **Enable Moonlight Debug Output** controls this
logging and is off by default. With debug enabled it writes
one `STREAM_DIAG_V1` line per second to the existing session `logfile`, including
when the statistics overlay is off or unavailable. Switch debug off to disable these samples. There is no separate health toggle. A direct native launch can opt in with
`MOONLIGHT_STREAM_DIAGNOSTICS=1`. This requires the updated native binary and
Luna source in the next add-on build; the published 2.7.1.301 package is unchanged.

`STREAM_DIAG_CONFIG_V1` records pipeline, requested dimensions, FPS and bitrate.
`STREAM_DIAG_START_V1` records wall-clock Unix seconds. Samples carry elapsed
`t_ms` and actual `dt_ms`; a delayed sampler is visible as a longer interval.
The first sample establishes baselines. Later `+` values describe that interval,
not lifetime totals. The existing `CodecWriteFail` field remains a lifetime count.

| Evidence | What it can help distinguish |
| --- | --- |
| `Host`, `RTT`, `Assembly`, video/FEC packet rates | Host processing, transport disruption or delayed assembly; the pinned video library does not populate recovery/OOS/error counters, so those fields show `n/a` |
| `cb`, callback age/gaps, `VQ`, `queue_mean_ms`, `queue20` | Video submission activity and time spent waiting in Moonlight's decoder queue; callback gaps alone do not prove a network fault |
| `Submit`, `submit20/50`, `EAGAIN`, `BPWait`, `BPDrop`, `QBufFail`, IDR and write failures | Slow AML submission, input backpressure, failed balanced-pacing buffer returns or reference recovery |
| `Dec`, `Sink`, decoder drops/errors and kernel queue | Decoder throughput versus the display sink's selected-frame changes; sink counts are not physical panel presents |
| `audio=alsa`, callback age/gaps, slow callbacks, PCM delay and its age, XRUN/recovery/failed-packet counts | Audio callback starvation, long writes/recovery, stale queue samples and underruns |
| `SysCPU`, `Th0` | Aggregate device CPU usage and thermal zone 0 temperature; neither alone proves throttling |

`cb_gap50/100` count intervals over 50/100 ms between video submit callbacks.
`submit20/50` count submit calls taking over 20/50 ms. `queue20` counts frames
waiting over 20 ms after common-c enqueues them. These fixed thresholds are
observations, not automatic diagnoses, and must be read alongside configured FPS.
In balanced pacing, a failed `VIDIOC_QBUF` increments `QBufFail`; Moonlight keeps
that dequeued buffer at the head of its local ring and retries it instead of
forgetting the index and shrinking the capture-buffer pool.

Audio `gap50` counts callback gaps over 50 ms; `slow20/50` count callback work
over 20/50 ms. `delay_ms` is signed ALSA PCM delay sampled on the audio thread
at most once per second. Missing/failed delay reads show `n/a`. Always inspect
`delay_age_ms`: an old value is not a current queue reading. PCM `state` uses
ALSA's numeric enum (2=PREPARED, 3=RUNNING, 4=XRUN, 7=SUSPENDED). Counters also
cover the optional ALSA resync path. A stalled callback leaves an increasing
`cb_age_ms` while the independent monitor continues sampling.

The AML detail counters are unavailable in the experimental VFM path and are
marked `native=n/a`; network/audio/system samples remain available. Missing
kernel measurements remain `n/a`. Scalars use lock-free 32-bit publication and
are approximate across concurrent callbacks. Counter deltas and timestamp ages
tolerate 32-bit wrap. Counters are reset for each session.

The monitor starts only after the connection initializes and stops before
connection teardown. Headless logging never opens DRM or claims a display plane.
With health diagnostics active, duplicate OSD text rows are suppressed; the
visual overlay still refreshes at its selected rate. There is no per-frame
health logging or new queue/recovery policy. Most rows are under
1 KB, with a fixed buffer limit of 2304 bytes per sample (under 8 MiB/hour at that
limit). That limit covers the added health rows, not all existing debug/error
messages. Existing rotation keeps the previous session; it is not a size cap
within a long session. Disable debug after collecting a reproduction.

After a slowdown, collect the usual CoreELEC log ZIP after exiting Moonlight.
Luna's current/previous session logs provide a timeline through the slowdown.
Compare the affected seconds with normal seconds on either side; do not infer
one cause from lifetime totals or one isolated counter.

Validation: host tests exercise the actual headless monitor with DRM calls
mocked, real ALSA callback code with PCM/Opus calls mocked, interval deltas,
idle stalls, stale/unavailable readings and u32 wrap. Strict host and ARM32
syntax checks cover the audio source, AML backend and monitor. ARM32 formatter
and actual audio-callback tests also execute under QEMU. These tests do not establish
target-device overhead or identify the existing slowdown without a new log.

The network fields were checked against the pinned common library's
`RtpVideoQueue.c`: only `packetCountVideo` and `packetCountFec` are updated.
Recovery/failure/OOS/invalid zeros previously shown by the OSD were unsupported
measurements, not proof of no loss. Existing common-library loss/RFI messages
still provide event evidence, but the client log alone cannot always distinguish
host capture trouble from packet loss. CPU/temperature do not prove throttling,
and no logging scheme can guarantee a diagnosis or zero overhead on hardware.

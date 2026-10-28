# Opt-in ALSA queue resynchronization (CoreELEC 21 NG)

The Vortex 2.7.1.277 NO executable's ALSA callback around
`0x410fb0–0x4111e0` checks `snd_pcm_avail_update`. When
available frames exceed the negotiated PCM buffer size, it uses
`snd_pcm_forward` to advance the application pointer (stale output
recovery), or drops/prepares the PCM stream if that cannot work.
It also retries `EAGAIN` with `snd_pcm_wait(..., 60)` and
handles partial writes and ALSA underruns. Those are actual ELF
observations, **not** proof of a video-frame pacing algorithm.

Our NG port adds an optional ALSA-only implementation, not a byte-for-byte
translation. Enable only by setting `MOONLIGHT_ALSA_RESYNC=1` in the child
Moonlight process environment. Luna exposes an Advanced toggle that
sets this variable for streaming sessions only, default **OFF**.
Leaving it disabled preserves the preexisting ALSA callback path.

The fast path checks ALSA's available frames and makes one direct write.
The corrective path forwards `excess + headroom` frames when
`avail > actual_buffer_size`; headroom is one negotiated period for
stereo and up to `buffer - period` for surround, based on the disassembly.
If the excess is implausibly large, a forward fails, or only partially
advances, the callback attempts one `snd_pcm_drop` + `snd_pcm_prepare`.
Write retries handle short writes without replaying PCM frames, and allow
at most 16 write attempts, two 60 ms waits, and two error recoveries
per packet to bound stalls in the audio callback. A cleanup summary
`ALSA_RESYNC_V1` includes forwarded frames, resets, timeouts, and
discarded frames. No per-packet diagnostic logging is added.

**Deliberate differences from Vortex:** the NG port retains the existing
20 ms requested ALSA period and 60 ms buffer for all channel counts.
Vortex's executable requests a different 50/200 ms configuration for
surround. That is a separate latency/compatibility decision, not part
of this queue-recovery port. Our port also retains existing CoreELEC
surround mapping and does not introduce an untested channel-map setter.

**Important: PulseAudio is unaffected.** `platform_get_audio()` tries
PulseAudio first and ALSA only on fallback; the resync feature has no
effect if PulseAudio succeeds or audio is played on the Sunshine host.
Check which backend was selected on the AM6B+ before benchmarking.

Testing: `cc -std=c99 -Wall -Wextra -Werror -pedantic -O2
tests/test_alsa_resync.c src/audio/alsa_resync.c -o /tmp/test_alsa_resync
&& /tmp/test_alsa_resync`. Mock cases cover no backlog, stereo/surround
forwarding, forward/reset failures, huge backlog, short writes,
`EAGAIN`, waiting, timeouts, underruns, and finite loop bounds.

No ARM32 native linking, ALSA hardware session, audio/video sync,
or Vortex performance equivalence has yet been established.
Test on a spare CoreELEC 21 NG AM6B+ with wired Sunshine H.264 1080p60,
then 5.1/7.1 if available; compare baseline (option OFF) against ON,
monitor audio pops/skips and resync counters, and confirm Kodi audio
restores on repeated launch/exit.

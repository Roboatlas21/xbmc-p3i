# CoreELEC 21 NG port

This branch starts at Moonlight Embedded commit `f7dc33c`, the upstream version
identified in Vortex's CoreELEC 22 NO Luna package.

## Target

- Hardware: Ugoos AM6B+ (Amlogic S922X-J).
- OS: Pannal's CoreELEC 21 NG (`PROJECT=Amlogic-ce`,
  `DEVICE=Amlogic-ng`, `ARCH=arm`).
- Native binaries must be built for the **32-bit ARM userspace** and against
  libraries from that CoreELEC tree. The supplied NO build is AArch64 and
  cannot be reused unchanged.
- Start with unmodified upstream Amlogic decoding. Do not assume Vortex's
  proprietary-to-this-build adjustments have been reconstructed.

## Build integration starting point

Pannal's CoreELEC tree already has:
`packages/addons/addon-depends/moonlight-embedded/package.mk`

That recipe currently fetches an older TheChoconut revision and applies
`01-surround-fix.patch`. When adapting the recipe to this source, enable
the equivalent **optional** mapping at CMake configuration time:

```sh
-DENABLE_CORE_ELEC_AUDIO_MAPPING=ON
```

Do **not** simultaneously apply `01-surround-fix.patch` to this fork;
its mapping is now available through the option, and the old context
would likely conflict. Leave the option OFF for non-CoreELEC builds.

The new build must also use compatible moonlight-common-c and libamcodec
headers/libraries. Keep the CoreELEC 21 sysroot and architecture together.

## Validation not yet performed

- [ ] Cross-compile using Pannal's CoreELEC 21 toolchain
- [ ] Verify built executable and libraries are ARM 32-bit ELF
- [ ] Resolve shared libraries against the NG image
- [ ] Pair with Sunshine and launch a 1080p60 H.264 desktop
- [ ] Verify Kodi returns after quit and failure
- [ ] Test stereo, 5.1 and 7.1 channel placement
- [ ] Test HEVC and higher resolutions after baseline works

This document does not indicate that NG playback currently works.

## Verified Vortex native compatibility toggle

Vortex's AArch64 decoder disables HEVC reference-frame invalidation when
`MOONLIGHT_HEVC_RFI=0` is present in the process environment. Our NG source
implements this as an opt-in override, preserving the upstream default.
This is a compatibility/recovery option, **not a proven latency reduction**.

The Vortex/Skupi-derived HEVC kernel parameters are now applied automatically
for HEVC streams when the kernel values exactly match the known NG defaults.
`MOONLIGHT_AML_TUNING_PROBE=1` remains a read-only VFM/HEVC diagnostic.
The old write-capable `MOONLIGHT_AML_VFM` override has been removed.
Frame routing is now selected only through the explicit Skupi-style
Latency/Balanced `codecParam.video_path` policies. See
`docs/vfm-hevc-experiments.md`.


## Skupi-style AML frame pacing

The NG AML backend supports `MOONLIGHT_FRAME_PACING=latency|balanced`.
The default is `latency`, matching Skupi's AML default.

- `latency`: uses `FRAME_BASE_PATH_TUNNEL_MODE` so the decoder feeds
  `amvideo` directly. This is compatible with the existing Vortex-derived
  direct-path work.
- `balanced`: uses `FRAME_BASE_PATH_AMLVIDEO_AMVIDEO` and an amlvideo pump
  that keeps at most three decoded frames waiting, drops excess queued frames,
  and returns one normal frame for each Amlogic control-fd refresh event.
The HEVC tuning, RFI controls and read-only VFM probe remain independent of
the selected pacing path. No mode rewrites the global VFM map.

The frame-pacing policy remains separate from the decoder timing controls below,
but both use the same explicit Amlogic decoder path selected for the session.


## Skupi/CoreELEC AML timing controls

The NG AML backend now uses the same core timing primitives observed in Skupi
and CoreELEC Kodi:

- `codec_checkin_pts_us64()` receives Moonlight's full microsecond PTS.
- `codec_pause()` is issued immediately after decoder initialization.
- `codec_set_cntl_mode(..., TRICKMODE_NONE)` selects normal playback mode.
- video delay accounting is capped at 1000 ms.
- AV threshold is 2,700,000 ticks (90 kHz × 30).
- sync threshold is 0.
- the existing freerun mode remains enabled afterward.

The native decoder does **not** modify `/sys/class/tsync`. Luna's CoreELEC
session wrapper now saves that global value, disables tsync while Moonlight owns
the video pipeline, and restores the exact original value before Kodi restarts.


## Amlogic decoder low-latency mode

During an AML stream, Moonlight temporarily enables
`/sys/module/aml_media/parameters/use_low_latency` when the CoreELEC kernel
exposes that parameter. The exact original boolean value is saved first and
restored after the decoder is closed or when setup fails partway through.

If the parameter is unavailable, unreadable, contains an unexpected value, or
cannot be changed safely, Moonlight logs the condition and continues without
the optimization. If it was already enabled before Moonlight starts, Moonlight
leaves it enabled and performs no restore write.

This low-latency lifecycle helper itself does not modify `/sys/class/tsync`,
VFM routing, or `codecParam.video_path`; those concerns remain independent and
the existing Latency/Balanced routing is unchanged.


## ALSA runtime diagnostics

The ALSA renderer keeps its existing low-latency 20 ms period / 60 ms requested
buffer policy. It does not adopt Skupi's more conservative startup buffering.

At stream startup, Moonlight logs the actual ALSA geometry negotiated with the
device as `ALSA_CONFIG_V1`, including sample rate, channels, period, buffer,
start threshold, and `avail_min`.

At cleanup it emits one `ALSA_STATS_V1` summary with write calls, EAGAINs,
XRUNs, recovery attempts/successes, short writes, failed packets, Opus decode
errors, and whether optional ALSA resynchronization was active. The counters are
updated in-memory only; there is no per-frame diagnostic logging.

## Streaming health log

Luna logs timestamped video/network/audio/system health once per second by
default, independently of the visual statistics overlay. See
[stream diagnostics](stream-diagnostics-coreelec-ng.md) for fields and interpretation.

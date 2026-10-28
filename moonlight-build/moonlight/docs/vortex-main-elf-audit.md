# Vortex main ELF: frame timing, input and audio (2026-10-25)

This is a **fresh** disassembly-based audit of the exact original Vortex
release, not an inference from promotional text. It complements the earlier
`docs/vortex-latency-reconstruction.md` analysis of the separately compiled
Amlogic decoder library.

## Evidence and methodology

* Artifact: `script.luna-2.7.1.277-f7dc33c8-NO-22.zip`
* SHA-256: `74c93039401b146b87fb67b62f95f1054834d48fba0bb5d88c64c639b78d0837`
* Examined object: `script.luna/bin/moonlight`, **stripped AArch64 ELF64**,
  336416 bytes, CoreELEC 22 NO, entry `0x405880`.
* Tools: `readelf -W`, `strings -a -t x`,
  `llvm-objdump -d --no-show-raw-insn`, `.eh_frame` FDE ranges for
  grouping calls to internal functions.
* Source comparison: upstream `f7dc33c87064e97f7f4e29457e312b9cceb24349`
  versus our `feat/coreelec-ng`. Current NG `src/input/evdev.c`,
  `src/input/udev.c` and `src/input/mapping.c` are **unchanged** from
  the upstream baseline. CoreELEC audio edits so far only change channel
  mapping.

The executable's function names have been stripped; the hex addresses below
are the actual virtual addresses observed, **not recovered source functions**.

## 1. Actual video presentation: no additional algorithm confirmed

| Executable evidence | Address / path | Interpretation |
| --- | --- | --- |
| `LiStartConnection` | `0x404e68` | Connection uses the usual decoder callbacks |
| `LiStopConnection` | `0x404ec4` | Ordinary stream shutdown |
| `drmModeAtomicCommit` | `0x40dfac` | Draws the independent native statistics **overlay**, not video frames |
| `usleep(50000)` | `0x407034` | Gamepad-to-mouse emulation, **already upstream** |
| `nanosleep` | `0x40b380` | Device rebind/wait logic, **not** a display pacing loop |
| `clock_gettime` | `0x40fe04`, `0x40fec8`, `0x4103f4` | Network round-trip measurement / diagnostics |
| `LiSendKeyboardEvent` | `0x407c44` and branch in CEC callback at `0x40b718` | Direct input-event forwarding |
| `LiSendMouseMoveEvent` | `0x407124`, `0x4079b4`, `0x407b90`, `0x407ea4`, `0x407eb8` | Input dispatch and mouse emulation |

No imports were observed for `sched_setscheduler`, `setpriority`,
`pthread_setschedparam` or `clock_nanosleep`. The executable imports
`nanosleep` once and uses it in a device-rebinding wait loop. That gives **no
positive evidence** of a dedicated user-space video presentation scheduler in
the main executable. A stripped binary cannot establish that an inlined or
indirect algorithm does not exist. The Amlogic library and NG/NO kernel drivers
remain the actual video-presentation components.

The previously examined `lib/libmoonlight-aml.so` shows the PTS conversion,
freerun, codec writes, VFM routing, and optional HEVC policy changes. It
does **not** establish that Vortex altered the legacy five EAGAIN retries or
the existing V4L2 dequeue/requeue policy. It would be unsound to claim a
frame-pacing speedup from this main executable audit alone.

## 2. Confirmed additional input-device recovery path

This is the most substantial previously undocumented **native input**
difference. The main executable invokes the input handoff routine at
`0x405278 -> 0x40b0c0` **before input-device selection and connection**.

Disassembly of `0x40aec0..0x40b050` enumerates input event devices,
queries their names via `ioctl`, and checks whether selected devices
remain held. `0x40b0c0..0x40b6e0` includes:

- inspection of `/sys/bus/usb/drivers/xpad/new_id`;
- USB `idVendor`/`idProduct` and interface class/subclass discovery;
- inspection of `/sys/bus/hid/devices/%s/driver`;
- sysfs **unbind/rebind** writes, plus attempts to teach the xpad driver
  an unrecognized USB ID;
- bounded retries and checks for release of chosen or all input devices.

The embedded diagnostic strings include
`input still held`, `input: teaching xpad about %s`,
`input: rebinding %s via %s`, and
`input: %d device(s) not free after %d s`.

This is **missing** from our NG native code. It may fix a controller that
Kodi or a HID driver holds when Moonlight starts. It is **not** direct proof
of lower USB-to-Sunshine event latency in steady-state streaming.
Because driver bind/unbind and `new_id` change global kernel state, do not
port these writes unconditionally or without identifying the exact affected
device and a rollback strategy.

The existing ordinary event conversion path in the Vortex executable
still calls `LiSendKeyboardEvent`, `LiSendMouseMoveEvent` and
`LiSendMultiControllerEvent` directly. No separate input batching or
frame-aligned mouse delivery algorithm was identified from these calls.

## 3. Confirmed ALSA queue latency/recovery differences

The Vortex audio callback at `0x410fb0..0x4111e0` differs from our
`src/audio/alsa.c` implementation:

- `snd_pcm_avail_update` at `0x41103c` and `0x4110b8` checks how much
  ALSA output space is available.
- `snd_pcm_forward` at `0x4110d8` advances the playback position when
  a backlog/overrun threshold is reached.
- After advancement the program logs
  `Audio: stream resynced, was %ld frames behind (%s)`.
- It retries writes with `snd_pcm_wait(..., 60)`, uses ALSA recovery,
  and can `snd_pcm_drop` and `snd_pcm_prepare` on a failing path,
  while restoring the audio channel map.
- Its initialization also configures ALSA start/stop/silence thresholds;
  calls observed around `0x4114c4..0x411558`.

Vortex still configures a **20 ms period and 60 ms buffer in the stereo
path** (around `0x4112d8..0x41130c`), which matches the existing upstream
initial values. The new feature is active buffer management and stale-queue
recovery, not simply a universally shorter ALSA buffer. Different channel
configurations require more disassembly before copying exact parameters.

**Impact:** credible improvement to stale *audio* latency and recovery,
not evidence of any reduced video frame-to-display time. Any port should
be scoped, test underruns/overflow, and preserve compatibility with Pannal's
ALSA stack and existing CoreELEC surround mapping.

## 4. Important correction: Vortex CEC volume is **local HDMI CEC**

Our recent CEC implementation in `ff992886` forwards CEC user-control
`0x41/0x42/0x43` as **Windows host** multimedia keys. This is not
Vortex's behavior.

Vortex's input event dispatcher routes local Linux input codes
`KEY_VOLUMEUP=115 (0x73)`, `KEY_VOLUMEDOWN=114 (0x72)`, and
`KEY_MUTE=113 (0x71)` to internal functions:

- `0x408610 -> 0x40c440`: calls libCEC `volume_up`.
- `0x407c8c -> 0x40c4d0`: calls libCEC `volume_down`.
- `0x408348 -> 0x40c560`: libCEC audio-system mute toggle; when none
  is detected, it issues CEC mute keypress/release toward the TV.
- libCEC is loaded dynamically and the receiver/AVR audio-system presence
  is checked during initialization (`0x40c100..0x40c424`).

The release prints either `CEC: ready, audio system on the bus` or
`CEC: ready, no audio system on the bus` and describes volume going to
the TV. The vendor `third_party/libcec/ceccloader.h` in our fork exposes
matching `volume_up`, `volume_down`, `audio_toggle_mute`,
`send_keypress` and `send_key_release` function pointers.

**Correction committed after this audit:** the NG evdev handler now
routes the local Linux media keys through libCEC when opted-in and the
adapter actually opens; HDMI-CEC-originated volume key codes no longer
generate Sunshine multimedia keys. Device testing is still required.
The distinction is supported by actual disassembly, not just symbol names.

## Conclusions / recommended order

1. **No new main-executable video frame-pacing algorithm confirmed.**
   Do not rewrite codec frame scheduling just to chase a speculative
   Vortex improvement; measure the existing native libamcodec/VFM path.
2. **Audio recovery is a concrete missing optimization.** Port behind a
   controlled, hardware-tested option rather than copy stale-buffer
   thresholds across incompatible ALSA devices.
3. **Input handoff is a concrete missing compatibility feature.**
   Start with read-only inspection of held devices; only consider
   per-device, safely reversible HID/xpad rebinding after device testing.
4. **Correct CEC volume semantics**: local TV/AVR control instead of
   host multimedia key injection.
5. Continue to measure input-to-photon latency at 1080p60 (H.264 first)
   on the same NG image with diagnostics disabled; an NO-vs-NG test has
   confounding differences in kernel and decoder stack.
6. `libmoonlight-common.so.4` from the Vortex ZIP was **not** separately
   reverse-diffed here; no claim is made about every network/queue strategy.

This audit changes the known native parity assessment. It does **not**
demonstrate any latency gain, verify NG hardware behavior, or contain
Vortex's original source code.

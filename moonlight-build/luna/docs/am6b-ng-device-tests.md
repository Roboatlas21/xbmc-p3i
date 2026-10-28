# AM6B+ CoreELEC 21 NG validation plan

This is a **manual device plan**, not a claim that device playback has been
tested. A native 32-bit Moonlight build and dependent libraries are required
before this can run.

## Preflight

- Use a secondary/test AM6B+ installation first.
- Keep Kodi/Arctic Fuse 3 and CoreELEC 21 NG running normally.
- Connect the Ugoos and Sunshine Windows host via wired Ethernet.
- Compile Moonlight for `PROJECT=Amlogic-ce DEVICE=Amlogic-ng ARCH=arm`
  using the recipe in `Roboatlas21/moonlight-embedded`.
- Confirm on the **build host** that the Moonlight executable and every
  provided shared library are ARM ELF32, not AArch64/ELF64.
- Place the executable at `script.luna/bin/coreelec-ng/moonlight`. When
  bundling private libraries, use `script.luna/lib/coreelec-ng/`. Avoid
  mixing the original NO binaries with NG.
- Check all runtime dependencies against the CoreELEC 21 sysroot/image.
- Do not change `/sys/class/vfm/map` or HEVC module parameters yet.
- After a stable baseline, enable **Advanced → Log Amlogic decoder timing**.
  Exit streaming and look for `AML_PROFILE_V1` in the Luna logfile.
  The summary covers submission, not hardware decode-to-display latency.
  Disable profiling for camera-based input-to-screen benchmarks.

## Playback and input checks

| Test | Action | Pass condition |
| --- | --- | --- |
| Pair | Luna > Pair with the Windows Sunshine host | PIN accepted; host saved |
| Applications | Open Applications and a custom folder/widget | Folder entries and box art display |
| H.264 baseline | Stream Sunshine Desktop at 1920×1080/60 | Stable image, no black screen |
| Quit | Leave streaming with the normal exit shortcut | Kodi/Arctic Fuse returns and responds |
| Host dropout | End the Sunshine connection mid-stream | Launcher resumes Kodi without forced reboot |
| Audio | Stereo, then 5.1 and 7.1 test tones if connected | Correct channel positions |
| Input | Type text, move/click mouse, use TV remote | Correct events, no stuck keys |
| Repeat | Start/stop five consecutive sessions | No stale audio devices or frozen Kodi |
| HEVC | Switch codec to HEVC with identical parameters | Stable picture and input |
| HEVC fallback | Repeat with `MOONLIGHT_HEVC_RFI=0` exported to child | Full-keyframe fallback behaviour |
| Higher bitrate | Gradually increase stream bitrate | No repeatable dropouts or decoder faults |
| 4K60 | Test only after stable 1080p60 | Stable and appropriately paced output |

On an NG-compatible build, the Luna launch script will use
`MOONLIGHT_BINARY` set to the architecture-specific executable.
The native HEVC fallback is **off by default**.

## Reconstructed native overlay and frame-pacing check

After the basic H.264 stream works, enable Luna's existing **OSD** toggle.
Its position, font size and interval are sent through Vortex-compatible
`osdargs` to the reconstructed native Moonlight executable.

The C-side implementation now has a separate worker that samples real
Amlogic frame-submission and V4L2-dequeue counters, >25 ms dequeue gaps,
network/FEC counters, host processing time, RTT and available kernel
queue values. **V4L2 dequeues are not verified screen presentations**.
With direct VFM presentation, the overlay shows `DQ n/a (direct)`.

If no ARGB8888 DRM overlay plane can be acquired because Kodi retains the
DRM master on NG, the video should continue without OSD. Enable the
ordinary **Debug** toggle as well as **OSD** to request periodic text
summaries in Luna's Moonlight logfile. The native code never steals an
in-use plane from Kodi. These graphics features have not been device-tested.

For latency comparisons, turn **OSD OFF** and disable the optional
decoder-timing profiler; graphics/diagnostic processing can itself
affect frame pacing. See
`Roboatlas21/moonlight-embedded/docs/native-drm-osd.md`.

## Experimental ALSA queue resynchronization

Luna's Advanced setting **Experimental ALSA audio resynchronization** is OFF
by default. It exports `MOONLIGHT_ALSA_RESYNC=1` only to streaming sessions.
The native ALSA callback detects a stale PCM buffer, advances its application
pointer when possible, and uses a bounded drop/prepare fallback, short-write
handling and underrun recovery.

Test using direct ALSA output. Moonlight prefers PulseAudio when it can open
the selected device: the toggle has **no effect** on that path, and none
when streaming audio stays on the Sunshine host. Start with 1080p60 H.264,
repeat five stream launches/exits and compare ON/OFF for pops, audio drift,
skips and volume. Inspect the final `ALSA_RESYNC_V1` log summary to confirm
the ALSA code path was used. Then test 5.1/7.1 mapping if the device supports it.

This only affects local audio queue recovery, not measured frame display
latency. Hardware testing and performance parity remain outstanding.

## Input-device ownership and guarded retry

Advanced **Log input-device ownership (read only)** defaults OFF. With it ON,
the native Moonlight executable scans other process descriptors for opened
`/dev/input/eventN` nodes and reports `INPUT_HANDOFF_V1`. A process having an
open file descriptor does not prove it has issued `EVIOCGRAB`; unreadable
processes may be omitted.

Expert **Retry unavailable input devices** also defaults OFF. On a failed
input-device open, Moonlight retries only `EBUSY`, `EAGAIN`, `EACCES`
up to three times (150 ms spacing). No global USB driver writes occur.
Even with both toggles enabled, Vortex's hard HID/xpad rebind remains unported.
Test selected controllers and auto-enumeration separately. Verify all input
devices keep working after normal Kodi resume and after host disconnect.

## Diagnostics to retain

1. CoreELEC version and `uname -a` (without usernames or IP addresses).
2. `/storage/.kodi/userdata/addon_data/script.luna/logfile`, after removing
   Sunshine host addresses, PINs, and private device identifiers.
3. Kodi log lines related to Luna/Moonlight/audio suspension, redacted.
4. Whether the TV shows video, audio, keyboard/mouse, and Kodi return.
5. Sunshine streaming statistics if available; compare *like for like*.

A successful source check or pairing alone does not prove decoder functionality,
latency, frame pacing, or safe HDMI output.

## Installation safety

This fork has not produced a device-tested ZIP. Do not overwrite the working
Pannal build or assume the original NO add-on is NG compatible.

## Independent Kodi recovery process

The default CoreELEC launcher now starts `kodi-watchdog.sh` before
sending SIGSTOP to `kodi.bin`. The watcher checks the launcher's
Linux `/proc/PID/stat` process start-time token **and zombie state**.
If the launcher disappears or becomes a zombie, it signals its verified
Moonlight child with TERM, waits briefly, then resumes Kodi with SIGCONT.

This addresses SIGKILL of the *launcher* (whose EXIT trap could not run),
but cannot recover a stuck kernel video driver, total system hang or
power failure. If Moonlight ignores TERM, Kodi resumes but another process
may still hold decoder/audio resources. It does not guarantee that the
framebuffer, VFM or CEC kernel state has been restored.

The ordinary launch path remains protected by the EXIT trap. Tests cover
normal/failed launches and simulated zombie recovery without stopping a
real Kodi process.

## Preflight before Kodi is suspended

For a bundled NG binary, Luna now validates the installed
`runtime-manifest.json`, the four native ELF objects and their SHA-256s,
and the SDL controller mapping. It then runs `moonlight help` with the
same private library directory as a normal launch. This verifies that
the **executable's** ARM32 dynamic loader and its immediate/transitive
DT_NEEDED libraries can start on the actual CoreELEC 21 device.

The check runs **before** stopping Kodi or suspending its audio. On an
incomplete/incompatible install it presents a diagnostic and does not
launch the stream. It does not validate the Amlogic decoder module's
lazy `dlopen` call, VFM/DRM permissions, or performance; those require
the first supervised stream. External/system Moonlight installations
and CoreELEC NO builds are left unchanged.

## Launcher scripts and watchdog readiness

The default `launch.sh`, `prescript.sh`, `postscript.sh` and
`kodi-watchdog.sh` use `#!/bin/sh`. Lua/Python subprocess calls
execute the scripts directly, so the interpreter path matters.

Before stopping Kodi, the rescue worker verifies the launcher's
`/proc` start-time token and writes a readiness acknowledgement.
If acknowledgement does not arrive within a short bounded window,
the launcher refuses to send SIGSTOP. Normal cleanup removes the
readiness file and rescues Kodi even if the command failed.

## Configuration path consistency

Bundled NG/NO executables now receive their settings in their **own
binary working directory** as `moonlight.conf`, before Luna considers
the global `/etc` fallback used for externally installed Moonlight.
Native Moonlight's `get_path` checks its current directory first and
does not otherwise search `/etc/moonlight.conf` directly. The NG ZIP
ships a blank placeholder there, which is rewritten from Kodi settings
on stream launch; pairing and app listing also refresh the configuration.
This avoids reaching Sunshine with stale/default settings and avoids
dependence on an unexpected `/etc` mount being writable.

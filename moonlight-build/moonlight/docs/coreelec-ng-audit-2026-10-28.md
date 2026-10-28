# CoreELEC NG branch audit — 2026-10-28

Scope: Moonlight `feat/coreelec-ng`, Luna `feat/vortex-ng`, and the public
`xbmc-p3i` build snapshot/workflow. Reviewed against the pinned CE21 NG 4.9 kernel
and the ARM32 userspace / ARM64 kernel contract. This audit updates source only;
it does not dispatch a build, release an add-on or change an installed box.

## Findings fixed

| Finding | Consequence | Correction |
| --- | --- | --- |
| VFM frames did not request the display driver's timestamp bypass | `vpts_expire()` still checks the media clock when tsync is disabled; frames could wait or depend on a global freerun setting left by a previous session | Bridge version 4 sets the existing `VFRAME_FLAG_GAME_MODE` on each frame. It retains version 3's amvideo DMA-device fix. This is separate from HDMI ALLM. |
| Audio initialization returned directly on ALSA errors and did not check Opus decoder creation | Partial allocations / an open PCM device could leak; a failed Opus allocation could leave a null decoder | Check decoder creation and unwind every acquired resource on initialization failure. Streaming buffers and the opt-in resynchronization policy are unchanged. |
| Only an already-loaded bridge was version-checked | A stale cached/bundled module could be accepted on the first launch | Collector requires version 4 and records it in the manifest; Luna validates the manifest, the module before loading, and the loaded version before probing the decoder. Unreadable loaded-version files produce a preflight error. |
| DMA-related exported-symbol checks were incomplete | A missing export could surface later in module build/linking | Add `of_find_device_by_node` and `dummy_dma_ops` to both module-build preflight lists. |
| Public preparation did not exercise native VFM worker ownership or the full shell launcher test | Recoverable source errors could survive until the expensive build/device test | Add host FFmpeg worker/frame tests, audio initialization failure injection and an ALSA syntax check; run the existing launcher recovery scenarios before the public native job. |

Evidence for the VFM timing correction is in pinned kernel
`2d0e0cc0db958d8d88f76407ac922f08d9a4004e`,
`drivers/amlogic/media/video_sink/video.c`, function `vpts_expire()`: the
per-frame game flag returns true before timestamp comparison. Disabling tsync
does not take that branch. The existing AML backend separately requests global
`FREERUN_NODUR`; VFM now uses the frame-local mechanism instead.

The pinned common library's `AudioStream.c` returns immediately when audio
initialization fails without calling the cleanup callback. The initializer
therefore owns cleanup of partial state.

## Offline validation

- All 62 Luna Python tests pass, including 18 VFM runtime tests and HDMI Game Mode
  restoration. The initial combined run hit two shell-test timeouts; the HDMI
  suite alone and the subsequent full suite both passed without source changes.
- The independent launcher shell suite passes: Kodi handoff, tsync, socket-buffer
  and CPU restoration, startup timeout and crash recovery.
- All 24 existing native source-check steps that do not install host packages
  pass, covering build/collector fixtures, module packaging, ALSA resync, AML
  tuning/refresh/profile, input, CEC and OSD. The separate DRM/OSD syntax checks
  pass against local libdrm headers. VFM packaging includes 20 tests.
- New VFM tests use real host FFmpeg allocation/reference counting with simulated
  decoder and bridge I/O. They cover overflow/IDR recovery, EAGAIN packet retry,
  malformed input, failed import, completion ownership, bad completion cookies,
  fatal worker errors and closing the bridge before releasing the final frame.
  Queue, descriptor, ABI and platform-selection tests also pass.
- New audio tests inject failures at 22 initialization points and retry after
  each; partial resources are released. The test fails against the pre-fix
  initializer. The complete ALSA source also passes strict syntax checks against
  real ALSA/Opus development headers, with and without CoreELEC audio mapping;
  an existing signedness mismatch at the Opus call was corrected. The failure
  harness uses mocked external APIs, not real ALSA hardware or a target build.
- The version 4 bridge compiles as an ARM64 kernel object against the pinned
  configured CE21 tree. This local check has no complete `Module.symvers`, so it
  is not a shippable-module link/CRC check.

## Later source changes (not covered by the offline test results above)

Subsequent commits added explicit post-connection readiness signaling, correct CLI
failure statuses, QBUF ownership retention, atomic AML display-thread stop
synchronization, and a Luna crash watchdog that snapshots/restores AML kernel
parameters and resets the video plane. Luna also changed Resume/Quit handling
and moved the NG add-on version to `2.7.1.302`; the public source snapshot
and Moonlight packaging pins were refreshed. These later changes have **not**
been covered by the historical passing test counts listed above.

## Remaining device validation

The previously published 2.7.1.301 runtime was unchanged at the time of this audit;
source now declares 2.7.1.302, which has not been validated as an installed runtime. Neither the version 3 DMA correction
nor the version 4 timestamp correction has been validated on the installed box.
The next complete build must still pass exact module symbol/CRC checks and the
ARM32 QEMU probe, then test first-frame presentation, normal exit, forced process
death, repeated launch and Kodi playback afterward. Host mocks cannot establish
DMA/cache behavior, display-IRQ teardown safety or latency.

The last log does not establish one cause for intermittent AML stutter. This
audit does not change bitrate, normal audio queue size or enable experimental
ALSA resynchronization by default. TV Game Mode remains on by default with saved
user preferences preserved. The add-on still uses the unchanged installed
CoreELEC kernel and device tree.

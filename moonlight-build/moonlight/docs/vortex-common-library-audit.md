# Vortex libmoonlight-common.so.4 — independent binary/source comparison

Artifact: `script.luna-2.7.1.277-f7dc33c8-NO-22.zip`,
SHA-256 `74c93039401b146b87fb67b62f95f1054834d48fba0bb5d88c64c639b78d0837`.
Examined object `lib/libmoonlight-common.so.4`: stripped AArch64
shared library with SONAME `libmoonlight-common.so.4`.

## Our source provenance

`Roboatlas21/moonlight-embedded:feat/coreelec-ng` has the submodule
`moonlight-stream/moonlight-common-c` pinned to
`b126e481a195fdc7152d211def17190e3434bcce` (2025-11-25).
The parent Moonlight baseline advertised in the original ZIP also cites
this gitlink. This establishes the intended source **revision**, not a
reproducible hash of Vortex's compiled binary.

## Direct binary observations

The shared library exports `RtpvAddPacket`,
`LiWaitForNextVideoFrame`, `LiGetPendingVideoFrames`,
`LiGetRTPVideoStats`, `queueRtpPacket`,
`sendInputPacketOnControlStream`, `enterLowLatencyMode`, and
`exitLowLatencyMode`. These are all part of upstream common-c's
public or internal protocols. The two exported Linux low-latency hooks
are **four bytes each** and contain an AArch64 `ret`, not a CPU
scheduler or OS thread-priority implementation.

At the exact pinned source commit:
- `src/VideoStream.c`: an ordinary receive thread and decoder
  thread, with `LiWaitForNextVideoFrame` preceding decode submission
- `src/VideoDepacketizer.c`: linked decode-unit queue and
  `LiGetPendingVideoFrames`
- `src/InputStream.c`: `MAX_QUEUED_INPUT_PACKETS=150`; 1-ms mouse
  batching constants and deferred send operations are *upstream*
- `src/Platform.c`: `initializePlatform()` calls
  `enterLowLatencyMode()`; for this Vortex Linux binary the hook is a
  no-op

Thus **no positively identified Vortex-specific input/network
pacing implementation** has emerged from exported API/function
layout and available strings. In particular, the 1-ms input batching
behavior is already present in our pinned source.

## Limits of this comparison

We do **not** possess a byte-identical AArch64 reference build compiled
with Vortex's compiler and linker flags. A stripped binary could contain
private/inlined modifications that leave exports and their names unchanged.
Function size comparisons against ARM32 or a different compiler would
be invalid. This is *API/provenance* parity, not proof of instruction parity.
To establish exact performance parity, obtain the source/patches or a
reproducible AArch64 control build and perform basic-block/constant
comparison, then profile queue delay on-device.

For repeatable inspection, run:
`python3 scripts/audit_vortex_common.py /path/to/script.luna-NO-22.zip`
with `readelf` and `llvm-objdump` installed. That script validates
the released archive digest and checks exported queue/input APIs and
the 4-byte stub hooks. It does not infer a full source diff.

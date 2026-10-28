# CoreELEC NG development

This fork is the source-only Kodi launcher for the CoreELEC 21 Moonlight port.

The original TheChoconut Luna source is retained. Vortex's 2026 NO package
(`script.luna-2.7.1.277-f7dc33c8-NO-22.zip`, with internal addon version
2.7.1.279) has been imported for the Kodi UI, settings, app folders, box art
and widget integration. Its compiled AArch64 player is not part of this NG source branch.

## First integration

- Choose native Moonlight based on the architecture of Kodi's Python process.
  Do not assume `uname -m` tells us whether userspace is 32-bit.
- Prefer `bin/coreelec-ng/moonlight` on 32-bit NG and
  `bin/coreelec-no/moonlight` on 64-bit NO.
- Reject an ELF executable with the wrong bitness rather than trying to run it.
- Use the chosen path for pairing, listing, quitting and launching sessions.
- Include the corresponding private library directory in `LD_LIBRARY_PATH`.
- Resume Kodi on normal exits and catchable signals, even on a failed launch.

## Status

This branch currently contains no compiled NG binary and is **not** an
installable, working NG distribution yet. A package assembler exists at
`packaging/package_ng.py`. The exact C decoder modifications are not
recoverable from the ZIP alone. The native fork has an optional,
diagnostic-only `MOONLIGHT_AML_PROFILE=1` timing summary.
In Luna, turn on Advanced → Log Amlogic decoder timing for the next stream,
and turn it off for input-to-display latency benchmarks.

## Validation

- [ ] Verify NG 32-bit executable selection on-device
- [ ] Verify NO 64-bit executable selection on-device
- [ ] Pair with Sunshine and stream 1080p60
- [ ] Test Kodi restoration after normal exit, crash and SIGTERM
- [ ] Test audio + input before repackaging

## VFM/HEVC experimental diagnostics

On a test AM6B+ installation only, Luna now exposes three independent
controls in Advanced/Expert settings. All three default to OFF.

- **Probe Amlogic pipeline (read only)** — records the current kernel VFM
  default chain and HEVC module parameters when starting Moonlight.
- **Experimental HEVC decoder tuning** — writes the two guarded HEVC values
  only for HEVC streams, if the NG kernel reports exactly the known defaults.
- **Experimental direct VFM video route** — switches the idle default chain
  from `decoder amvideo` to a direct H.264 or HEVC provider. It refuses
  unexpected or active VFM routes.

The native source checks for an exact match, and tries to restore original
values after normal exit. SIGKILL, power loss and system crashes prevent
guaranteed restoration of global kernel settings. Keep the two experimental
controls OFF on your primary CoreELEC installation.

See `Roboatlas21/moonlight-embedded:docs/vfm-hevc-experiments.md` for
the kernel evidence, expected values, failure recovery, and benchmark matrix.

No feature parity or end-to-end latency result is implied.

## NG launcher and packaging audit

The default `coreelec/launch.sh` now uses `#!/bin/sh`, and
`test_launch.sh` executes the script directly, validating the shebang
and executable permission rather than masking a bad interpreter.

The NG test ZIP includes only the `resources/launchscripts/coreelec`
launcher. Legacy Vortex `coreelec_usb`, `coreelec_usb_new` and other
presets are preserved in source for comparison, but some globally
deauthorize every USB device or change display state and are not safe
defaults for CoreELEC 21. Existing saved choices pointing to missing
scripts fall back to `coreelec` during Luna initialization.

The source-only fixes do not establish ARM32 playback or Kodi return
under crashes; actual NG device validation remains necessary.

### Stream setup failure status

Moonlight now returns a nonzero process status when `LiStartConnection()`
fails instead of falling through to a successful process exit. The NG launcher
preserves that exact child status, records when Moonlight exits before the
post-connection ready marker, and restores Kodi and session-scoped system state
through the existing cleanup path.

### Confirmed Resume state

Luna no longer writes `last_run` before preflight or service handoff. On
CoreELEC NG, the independent launcher updates Kodi's `last_run` setting only
after Moonlight publishes the post-`LiStartConnection()` ready marker. Failed
preflight, failed systemd dispatch, connection failure and startup timeout leave
the previous Resume entry unchanged. Other launchers update `last_run` only
after a zero exit status. Explicit Resume launches keep the existing value.

### Balanced pacing buffer ownership

The standard AML balanced-pacing mode now requires a Moonlight build that only
advances its local amlvideo ring after `VIDIOC_QBUF` succeeds. A failed kernel
requeue keeps the dequeued buffer owned by Moonlight for a later retry instead of
silently shrinking the capture-buffer pool for the rest of the session. With
Moonlight debug output enabled, interval diagnostics expose these attempts as
`QBufFail`. Luna continues to pass `MOONLIGHT_FRAME_PACING=balanced` unchanged;
the fix itself is in the native Moonlight branch and therefore requires the next
native package.

## Persisted Moonlight device identity

Luna's direct HTTPS helper now reads `uniqueid.dat` with the same 16-byte
semantics as the native Moonlight client. A valid persisted custom ID is reused,
trailing data is ignored after byte 16, and a missing or short file falls back
to `0123456789ABCDEF`. This keeps application-list, pairing-status and box-art
requests under the same device identity used by moonlight-embedded.

## Kodi add-on path normalization

Kodi may return the add-on installation directory with or without a
trailing path separator. `ConfigHelper` now normalizes it once during
initialization; otherwise the existing path concatenations for
`resources/launchscripts`, the intro video, and a fallback configuration
path could incorrectly target `script.lunaresources/...`. Tests cover
both Kodi path styles.

## Native runtime manifest contract

New CoreELEC NG packages now require `runtime-manifest.json` in the
native ARM32 tarball. The packager verifies the manifest's target, ABI,
per-object SHA-256/size, expected SONAMEs, declared dependencies and
dynamic linker path against the actual included native bytes. This
prevents silently combining an old Pannal library with a newer Moonlight
player or accidentally packing an AArch64 executable.

For an earlier build with no manifest, `--allow-legacy-bundle` permits
**explicit** ELF-only test packaging; it is not the default and offers
weaker provenance checks. The collector and packager agree on the
four private objects. Their remaining `external_needed` libraries
must still be verified against the actual CoreELEC 21 device; manifest
validation alone does not establish that the runtime loader can find them.

## Controller database included in NG package

The native artifact now includes and hashes the pinned
`bin/coreelec-ng/gamecontrollerdb.txt` file. Moonlight's default
`get_path()` checks the working directory first, and Luna executes
Moonlight from its architecture-specific `bin/coreelec-ng/` directory.
Without this asset, the standalone ZIP could fail to initialize evdev
mappings on a box that does not have the OS-wide SDL database installed.
The manifest validates both the input data and the four ELF objects.

Vortex's Amlogic module is loaded through `dlopen` and may depend
directly on `libamcodec.so` without an additional `libgamestream`
DT_NEEDED edge. Both native and Luna validators correctly permit this,
while preserving strict SONAME, SHA-256 and executable dependency checks.

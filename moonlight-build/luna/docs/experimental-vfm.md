# Optional ARM32 FFmpeg / VFM prototype

Advanced settings → **Video pipeline** selects either the existing **Amlogic
(standard)** backend (default) or **FFmpeg / VFM (experimental)**.

The prototype requires a Moonlight build with `ENABLE_AML_VFM=ON`, private patched
FFmpeg libraries, a matching `moonlight_vfm.ko`, and compatible stateful V4L2
decoder modules. The kernel module is ARM64; the app and its libraries remain
ARM32. This does not replace CoreELEC, Kodi, or the device tree.

Build instructions and limitations live in Moonlight's
`docs/experimental-vfm.md`. This is source-stage work, not a hardware-validated
release. Initial support is SDR H.264/HEVC; turn off HDR and the statistics overlay.

Package an existing standard native bundle with the optional verified directory:

```sh
python3 packaging/package_ng.py /path/moonlight-coreelec21-ng-arm32.tar.gz \
  --vfm-directory /path/vfm-runtime --output /path/outside/checkout/luna-vfm.zip
```

The optional directory has its own manifest with hashes, ARM32 library checks,
ARM64 module checks, the exact kernel release and module load order. Preflight
checks the bundle and loads the userspace plugin before stopping Kodi. The
independent launcher loads modules after Kodi stops; it leaves modules loaded
between games. On errors, its existing cleanup/watchdog restores Kodi.

A standard package still installs without this directory. Selecting the prototype
then shows a dependency error and leaves Kodi running. It does not silently fall
back to AML. Return the setting to **Amlogic (standard)** to use the existing path.

No GitHub Actions workflow is required to select or locally build this option.

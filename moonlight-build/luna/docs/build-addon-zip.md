# Assemble a CoreELEC 21 NG Luna test ZIP

This combines two independent private repositories **without giving either
repository access to the other's GitHub credentials**.

## Prerequisite: successful native ARM32 build

Download the `moonlight-coreelec21-ng-arm32` artifact from
`Roboatlas21/moonlight-embedded` Actions →
**CoreELEC 21 NG ARM32 native build**. Extract the artifact ZIP. It contains:

- `moonlight-coreelec21-ng-arm32.tar.gz` (native executable + its three
  Moonlight-specific shared libraries).
- `build-info.txt` (ELF/SONAME/DT_NEEDED report).

If the native GitHub Actions run failed or produced no artifact, **stop**.
Do not use Vortex's 64-bit NO package in its place.

## Build the ZIP from your Luna checkout

Check out `Roboatlas21/Luna` branch `feat/vortex-ng` and run:

```sh
python3 packaging/package_ng.py \
  /path/to/moonlight-coreelec21-ng-arm32.tar.gz \
  --output /tmp/script.luna-CoreELEC21-NG-test.zip
```

The packager:

1. Rejects ELF64/AArch64 native files and unexpected archive paths.
2. Includes Luna's Python, media, icons, localization, and GPL license.
3. Bundles the executable under `script.luna/bin/coreelec-ng/` and the
   Moonlight-specific libraries under `script.luna/lib/coreelec-ng/`.
4. Increments the add-on version for this **test** ZIP and labels it as NG.
5. Preserves executable permissions for Moonlight and shell scripts.

It intentionally does **not** include CoreELEC's own `libamcodec.so` or
glibc. Those must be supplied by CoreELEC 21 NG.

The resulting ZIP should have a `script.luna/` top-level directory, as Kodi
expects. It replaces add-on ID `script.luna` if installed, so use your
secondary/test installation first.

## Checks before installing

```sh
unzip -t /tmp/script.luna-CoreELEC21-NG-test.zip
unzip -l /tmp/script.luna-CoreELEC21-NG-test.zip | grep coreelec-ng
```

A successful cross-build and ZIP check **do not** prove runtime
compatibility. The AM6B+ still needs Sunshine pairing, 1080p60 H.264,
HEVC, audio, input, exit/restart and latency validation per
`docs/am6b-ng-device-tests.md`.

There is currently no automatic cross-private-repository publishing pipeline:
that would require a narrowly scoped GitHub App token or a deliberate
manual artifact handoff. No token is required for the workflow above.

# Read-only VFM diagnostics and HEVC decoder tuning

The native Moonlight Amlogic backend keeps the Vortex/Skupi-derived HEVC
decoder tuning, but no longer rewrites the global VFM map.

Frame routing is now selected exclusively through the Skupi-style
`MOONLIGHT_FRAME_PACING=latency|balanced` policy:

- `latency` uses `FRAME_BASE_PATH_TUNNEL_MODE`.
- `balanced` uses `FRAME_BASE_PATH_AMLVIDEO_AMVIDEO` and the paced
  amlvideo pump.

This removes the old `MOONLIGHT_AML_VFM` override and its global
`/sys/class/vfm/map` mutation/rollback risk.

## Remaining environment control

| Variable | Value | Effect |
|---|---|---|
| `MOONLIGHT_AML_TUNING_PROBE` | `1` | Read-only: report the active VFM route and HEVC parameter values |

For every HEVC stream, guarded HEVC tuning changes:

- `/sys/module/amvdec_h265/parameters/nal_skip_policy` to `0`
- `/sys/module/amvdec_h265/parameters/ref_frame_mark_flag` to
  `0,0,0,0,0,0,0,0,0`

It is applied automatically only when the original values exactly match the
known NG defaults (`2` and nine `1` values). Original values are restored on
normal cleanup and on partial setup failures. A forced kill or power loss can
still prevent restoration because these HEVC parameters are global kernel state.

The VFM probe remains useful for confirming the effective decoder route, but it
is strictly read-only.

## Host test

```sh
cc -std=c99 -Wall -Wextra -Werror -pedantic -O2 \
  tests/test_aml_tuning.c src/video/aml_tuning.c -o /tmp/test_aml_tuning
/tmp/test_aml_tuning
```

The mock verifies that the VFM probe performs no writes, HEVC tuning applies
automatically only to HEVC, unknown defaults are refused, partial writes roll
back, and external changes are not overwritten during restore.

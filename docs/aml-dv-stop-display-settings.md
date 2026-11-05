# Avoid flash persistence during DV teardown

`aml_dv_off()` previously restored HDMI color attributes through
`write_resolution_ini()`. That also rewrote `/flash/resolution.ini` and, on a
read-only flash mount, remounted it twice while holding the DV-core lock.

DV teardown now calls `aml_apply_hdmi_color_attributes()`. Both this runtime
path and the existing persistence path use the same chroma/depth formatter.
The runtime write is deliberately repeated even when settings are unchanged:
the DV pipeline can have changed the live HDMI state. Resolution and display
settings callbacks continue to persist the boot configuration.

The bypass toggle, disable sequence, HDMI reevaluation, VPP display-mode reset,
native GUI OSD restoration, and `skip_hdmi_update` gate remain in place. This
patch does not combine display-mode transitions or shorten hardware waits.

The motivating trace showed about 545–642 ms inside DV close, including a
resolution-file save. That interval includes other hardware work; the cost of
the file operation was not isolated. No speedup is claimed before device tests.

## Automated checks

Run `python3 tools/test-aml-dv-display-settings.py --negative-controls` with a
C++17 compiler available (`g++` by default, or set `CXX`). The harness compiles
the production functions with simulated settings/sysfs/mount operations and a
real temporary configuration file. It checks color policies, repeated runtime
writes, DV-off ordering and skip paths, and persistent configuration fields.
Negative controls reintroduce the flash write or remove the live write/reset
to verify that the checks detect those regressions.

These checks do not build the complete Amlogic ARM32 binary or emulate a TV.

## Device comparison

Keep the existing binary for rollback and build from this branch using the
same CoreELEC toolchain as the skin reload restoration build. Repeat the same
episodes with the media drives already awake. Compare DV-close and stop-to-GUI
log markers, and check the picture on the TV.

Exercise SDR, native DV and HDR through VS10; stop with both matching and
different GUI/playback refresh rates. Also check always-on DV, immediate
replay, and skin reload restoration. Verify correct GUI colors and geometry.
On the DV-off path the resolution-save log should disappear, while the live
attribute write and VPP reset still run. A later resolution/settings change
must continue to update `resolution.ini`, including the final GUI mode.

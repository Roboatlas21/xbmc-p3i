# Vortex ALSA PCM fidelity audit — verified AArch64 instructions

Artifact: original Vortex Luna NO 2.7.1.277 ZIP, SHA-256
`74c93039401b146b87fb67b62f95f1054834d48fba0bb5d88c64c639b78d0837`.
Examined `bin/moonlight` with `llvm-objdump` (AArch64 disassembly).
The addresses identify exact instruction sequences, **not** recovered source.

| Operation | Vortex disassembly | Our NG ALSA |
| --- | --- | --- |
| Stereo geometry | `0x4112d8–0x41130c`: period 20 ms, buffer = 3 periods (60 ms) | 20/60 ms |
| Surround geometry | `0x4115bc–0x4115c4`: period 50 ms, buffer = 4 periods (200 ms) | 20/60 ms |
| Start threshold | `0x4114ac–0x4114c4`: period for stereo, entire buffer for surround | period for all |
| Available minimum | `0x411494–0x4114a0`: one period | one period |
| Stop threshold | `0x4114fc–0x411520`: ALSA boundary | ALSA defaults |
| Silence threshold | `0x411530–0x41153c`: zero | ALSA defaults |
| Silence size | `0x41154c–0x411558`: ALSA boundary | ALSA defaults |
| Channel map | `0x411624–0x4116b4`: dynamically allocates and applies `snd_pcm_set_chmap()` for >2 channels | Opus channel mapping only |
| Channel map recovery | `0x41118c–0x41119c`, `0x4111b4–0x4111c4`: attempt to restore saved map on recovery | Not implemented |
| Queue resync | `0x41103c–0x4111d8`: avail/forward/drop/prepare/wait/recover | Reconstructed behind `MOONLIGHT_ALSA_RESYNC=1`, OFF by default |

The period/buffer sizes above are **requested**. The hardware may negotiate
slightly different actual values. Our code now reads negotiated period/buffer
values for the optional resync logic.

**Interpretation:** Vortex prioritizes avoiding surround underruns and output
stalls, even with a larger queued-audio allowance. His larger surround
buffer is not evidence of lower audio latency; it can trade latency for
stability. His channel-map handling may improve HDMI 5.1/7.1 routing but
must be checked against Pannal's libasound and CoreELEC channel ordering.

**Decision:** keep our 20/60 ms period/buffer and CoreELEC mapping as the
default. Do **not** force 50/200 ms or change start/stop/silence thresholds
just to imitate Vortex without a measured benefit. Avoid new `snd_pcm_set_chmap`
writes until the exact channel positions are confirmed on the AM6B+.
These details are source-level compatibility gaps, not confirmed playback bugs.

Device test: H.264 1080p60, same output route/AVR and Sunshine settings;
compare stereo, 5.1 and 7.1 with resync OFF and ON; record `ALSA_RESYNC_V1`,
alsa status/underruns, A/V drift, and Kodi audio restoration. Note that
Moonlight tries PulseAudio before ALSA, so confirm actual backend selection.

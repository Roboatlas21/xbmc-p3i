# CEC volume input on CoreELEC 21 NG

Luna's `cec = true/false` option is now honored by the native parser;
the ARM32 build enables its Pannal `libcec` dependency. It remains
**OFF by default**.

The Vortex ARM64 main executable was subsequently disassembled, revealing
that the first host-volume implementation was incorrect. Actual Vortex
routes *Linux evdev media keys*, not HDMI CEC volume messages, to libCEC:

| Linux key event | Vortex / current action |
| --- | --- |
| `KEY_VOLUMEUP` (115) | libCEC `volume_up` to local CEC audio target |
| `KEY_VOLUMEDOWN` (114) | libCEC `volume_down` |
| `KEY_MUTE` (113) | audio-system mute toggle or CEC mute press/release to TV |

The handlers are gated on a successfully opened CEC adapter. If CEC is off
or cannot open while Kodi holds the device, normal input forwarding is
unchanged; no false claim of volume control is made. Key-up is consumed
only while CEC owns those local media keys. Audio-system presence is
probed once after opening. The **CEC-originated navigation** callback
still forwards arrows and select to Sunshine; it does not inject host
Windows volume keys.

Vortex may attempt to activate its own HDMI source; this fork explicitly
does not, to avoid changing the TV input while Kodi is suspended.

Evidence: `docs/vortex-main-elf-audit.md`, main ELF addresses
`0x407c8c`, `0x408348`, `0x408610`,
`0x40c440`, `0x40c4d0`, `0x40c560`.

Manual hardware validation on a spare AM6B+:
1. Check that Sony remote volume still works when Luna CEC is off.
2. Enable Luna CEC and verify remote media keys target Sony/AVR,
   not Windows sound volume or the Moonlight application.
3. Repeat without an AVR (TV fallback) and with an AVR if available.
4. Confirm CEC adapter opening under Kodi SIGSTOP and normal handling
   after five start/stop cycles.
5. Check key-down/repeat and mute toggle do not double-trigger.
6. Confirm disconnects or missing CEC devices do not block streaming.

No real CoreELEC 21 CEC runtime testing has been performed.

# Streaming health logging

Advanced settings → **Log streaming health** is on by default. Once per second,
Luna records streaming health in its normal session log, including with the
statistics overlay off. It covers AML video submission/decoder queues, transport
recovery, ALSA callback timing/queue/errors, CPU load and temperature. VFM has
network/audio/system measurements; AML-specific fields show unavailable.

The next add-on build must include the updated Moonlight binary. This source
change does not update the installed or published 2.7.1.301 package.

After a slowdown, exit Moonlight and collect the usual CoreELEC log ZIP. The
current/previous Luna session logs include a timestamped timeline around the
incident. You can disable the new samples with the toggle. Decoder timing summary
and experimental audio resynchronization remain separate settings.

The complete field reference and limitations are in Moonlight's
`docs/stream-diagnostics-coreelec-ng.md`. Missing readings are unavailable rather
than inferred. A log can distinguish likely areas to investigate; it cannot
prove every cause from one counter.

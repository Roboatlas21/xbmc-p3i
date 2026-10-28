#!/usr/bin/env python3
"""Compare opt-in Amlogic submission diagnostics from two Kodi/Moonlight logs.

Usage:
  python3 scripts/compare_aml_profiles.py baseline.log candidate.log

The metrics are NOT end-to-end streaming latency or decoder presentation delay.
"""
import argparse
import re
from pathlib import Path

PREFIX = "AML_PROFILE_V1 "
KEY_VALUE = re.compile(r"([a-z0-9_]+)=(\d+)")


def read_latest(path):
    sessions = []
    for line in Path(path).read_text(errors="replace").splitlines():
        marker = line.find(PREFIX)
        if marker < 0:
            continue
        fields = {name: int(value) for name, value in
                  KEY_VALUE.findall(line[marker + len(PREFIX):])}
        if "frames" in fields and fields["frames"]:
            sessions.append(fields)
    if not sessions:
        raise ValueError("No AML_PROFILE_V1 session in " + str(path))
    return sessions[-1]


def format_value(name, profile):
    value = profile.get(name)
    if value is None:
        return "n/a"
    if name == "submit_p95_bucket_us" and value == 0:
        return ">132000"
    if name == "eagain_per_frame":
        return "%.3f" % (profile["eagain"] / profile["frames"])
    return str(value)


def compare(baseline, candidate):
    fields = (
        ("frames", "Frames submitted"),
        ("submit_mean_us", "Mean submit / frame (us)"),
        ("submit_p95_bucket_us", "P95 submit bucket upper bound (us)"),
        ("submit_max_us", "Worst submit / frame (us)"),
        ("write_mean_us", "Mean write+retry / frame (us)"),
        ("assembly_mean_us", "Mean network assembly / frame (us)"),
        ("eagain_per_frame", "Codec EAGAIN per frame"),
        ("failed", "Submission failures"),
        ("v4l2_dqbuf_cycles", "V4L2 dequeue cycles (not displayed frames)"),
    )
    output = ["Metric | Baseline | Candidate", "--- | ---: | ---:"]
    for name, label in fields:
        output.append("%s | %s | %s" % (
            label, format_value(name, baseline), format_value(name, candidate)))
    output.extend([
        "",
        "These opt-in measurements can help diagnose stalled submissions.",
        "They DO NOT measure end-to-end latency, actual Amlogic decode time,",
        "or the presentation-to-display interval. Run an independent",
        "same-input high-speed-camera benchmark with profiling disabled",
        "for true input-to-screen latency comparisons.",
    ])
    return "\n".join(output)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("baseline", type=Path)
    parser.add_argument("candidate", type=Path)
    args = parser.parse_args()
    print(compare(read_latest(args.baseline), read_latest(args.candidate)))


if __name__ == "__main__":
    main()

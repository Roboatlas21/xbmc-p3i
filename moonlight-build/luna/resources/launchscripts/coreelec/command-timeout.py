#!/usr/bin/env python3
"""Bound Luna recovery commands without depending on CoreELEC's shell tools."""
import math
import os
from pathlib import Path
import signal
import subprocess
import sys


class Interrupted(Exception):
    def __init__(self, signum):
        self.signum = signum


def descendants(pid):
    """Record identities before stopping a parent can reparent its children."""
    children = {}
    identities = {}
    # /proc/PID/task/TID/children requires a kernel option CoreELEC need not
    # enable. The ordinary stat record always supplies PPID and start time.
    for entry in Path('/proc').iterdir():
        if not entry.name.isdigit():
            continue
        try:
            stat = (entry / 'stat').read_text().rsplit(') ', 1)[1].split()
            current = int(entry.name)
            identities[current] = stat[19]
            children.setdefault(int(stat[1]), []).append(current)
        except (OSError, IndexError, ValueError):
            continue
    found = {}
    pending = [pid]
    while pending:
        current = pending.pop()
        if current in found or current not in identities:
            continue
        found[current] = identities[current]
        pending.extend(children.get(current, []))
    return found


def kill_recorded(processes):
    for pid, started in processes.items():
        try:
            stat = Path('/proc/%d/stat' % pid).read_text().rsplit(') ', 1)[1].split()
            if stat[19] == started:
                os.kill(pid, signal.SIGKILL)
        except (OSError, IndexError):
            pass


def run(command, seconds, grace):
    try:
        child = subprocess.Popen(command, start_new_session=True)
    except OSError as error:
        print('Luna NG: cannot run %s: %s' % (command[0], error), file=sys.stderr)
        return 127 if isinstance(error, FileNotFoundError) else 126

    def signal_group(signum):
        try:
            os.killpg(child.pid, signum)
        except ProcessLookupError:
            pass

    def interrupted(signum, _frame):
        raise Interrupted(signum)

    def reap():
        # An uninterruptible kernel operation can outlive SIGKILL.
        try:
            child.wait(timeout=0.2)
        except subprocess.TimeoutExpired:
            pass

    previous = {sig: signal.signal(sig, interrupted)
                for sig in (signal.SIGTERM, signal.SIGINT, signal.SIGHUP)}
    try:
        try:
            result = child.wait(timeout=seconds)
            return result if result >= 0 else 128 - result
        except subprocess.TimeoutExpired:
            owned = descendants(child.pid)
            signal_group(signal.SIGTERM)
            try:
                child.wait(timeout=grace)
            except subprocess.TimeoutExpired:
                pass
            # The direct child may exit on TERM while its descendants survive.
            signal_group(signal.SIGKILL)
            kill_recorded(owned)
            reap()
            print('Luna NG: command timed out after %gs: %s' %
                  (seconds, command[0]), file=sys.stderr)
            return 124
    except Interrupted as error:
        # An enclosing recovery deadline must not orphan a nested command group.
        owned = descendants(child.pid)
        signal_group(signal.SIGKILL)
        kill_recorded(owned)
        reap()
        return 128 + error.signum
    finally:
        for sig, handler in previous.items():
            signal.signal(sig, handler)


def main(argv):
    try:
        grace = 2.0
        if argv[:1] == ['-k']:
            grace = float(argv[1])
            argv = argv[2:]
        seconds = float(argv[0])
        command = argv[1:]
        if (not command or not math.isfinite(seconds) or seconds <= 0 or
                not math.isfinite(grace) or grace < 0):
            raise ValueError('invalid deadline')
    except (IndexError, ValueError):
        print('Usage: command-timeout.py [-k GRACE] SECONDS COMMAND [ARGS...]',
              file=sys.stderr)
        return 2
    return run(command, seconds, grace)


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))

#!/usr/bin/env python3
"""Refuse to start a native-renderer run when the GPU is in no state to take one.

WHY THIS EXISTS
---------------
On 2026-08-12 this project's native SDL3-GPU render path hung the graphics ring repeatedly.
amdgpu named `sms-recomp` on seven ring timeouts in one session, reset the device each time, and
the desktop session went down with it — twice, the second time hard enough to need a reboot.

Every one of those resets was avoidable. The first device loss is a stop signal, and the runs
after it were started anyway: nothing in the harness knew the card had just been reset, so each
new run looked exactly like the first. A GPU hang is not a crash you can iterate through — the
fault belongs to the whole card, and the process that loses is whichever one is drawing the
user's desktop.

So this is an INTERLOCK, not advice. `run-render.sh` calls it and exits if it says no.

WHAT IT CHECKS
--------------
1. amdgpu ring timeouts / resets / "device wedged" in the kernel log within a cooldown window.
   A card that was reset minutes ago is still settling; the runs immediately after a reset were
   the ones that failed at SDL init and, eventually, took the session down.
2. A stamp file written by the runtime itself the moment it disables the renderer after a GPU
   fault (`scratch/gpu_fault.stamp`). This works even where the kernel log is unreadable, and it
   records our OWN view of the failure rather than inferring it.

Neither check can pass vacuously: if the kernel log cannot be read at all, that is reported as
UNKNOWN and treated as a refusal unless overridden, because "I could not look" and "I looked and
it was clean" must never produce the same answer.

OVERRIDE
--------
`--force`, or SBR_GPU_PREFLIGHT=off. Both print what they are overriding. The override exists
because the user owns the machine and may know the card is fine; it is deliberately not silent.
"""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
import time
from datetime import datetime
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
STAMP = REPO / "scratch" / "gpu_fault.stamp"

# How long after a GPU reset this refuses to start a render run. Chosen from observation, not
# taste: after each reset in the 2026-08-12 session, runs started within a few minutes failed at
# SDL init ("XIO: fatal IO error 2") and the display session needed longer than that to settle.
COOLDOWN_SECS = 15 * 60

TROUBLE = re.compile(
    r"amdgpu.*(ring .* timeout|ring reset|device wedged|GPUVM fault|reset succeeded)"
    r"|drm.*(GPU reset|reset begin)",
    re.IGNORECASE,
)


def kernel_log(minutes: int) -> tuple[list[str], str | None]:
    """Return recent matching lines without relying on journalctl's broken time windows."""
    try:
        out = subprocess.run(
            ["journalctl", "_TRANSPORT=kernel", "--no-pager"],
            capture_output=True, text=True, timeout=30,
        )
    except FileNotFoundError:
        return [], "journalctl is not installed"
    except subprocess.TimeoutExpired:
        return [], "journalctl timed out"
    if out.returncode != 0:
        return [], f"journalctl exited {out.returncode}: {out.stderr.strip()[:200]}"

    # The wall clock stepped backward during this boot. journalctl --since has been measured to
    # return either the entire history or no lines against that inverted range, so fetch once and
    # filter in-process. An unparseable timestamp is kept: uncertainty must refuse the run, not be
    # converted into a clean result.
    cutoff = time.time() - minutes * 60
    months = {
        name: index + 1
        for index, name in enumerate(
            ("Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec")
        )
    }
    hits: list[str] = []
    for line in out.stdout.splitlines():
        if not TROUBLE.search(line):
            continue
        parts = line.split()
        try:
            hour, minute, second = map(int, parts[2].split(":"))
            stamp = datetime(
                datetime.now().year,
                months[parts[0]],
                int(parts[1]),
                hour,
                minute,
                second,
            )
            if stamp.timestamp() < cutoff:
                continue
        except (IndexError, KeyError, ValueError):
            pass
        hits.append(line)
    return hits, None


def check_stamp() -> str | None:
    """The runtime's own record of a GPU fault. Returns a reason to refuse, or None."""
    if not STAMP.exists():
        return None
    age = time.time() - STAMP.stat().st_mtime
    if age > COOLDOWN_SECS:
        return None
    why = STAMP.read_text(errors="replace").strip()[:300]
    return (f"the runtime recorded a GPU fault {int(age // 60)}m{int(age % 60)}s ago "
            f"(scratch/gpu_fault.stamp): {why}")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--force", action="store_true", help="run anyway, printing what is overridden")
    ap.add_argument("--cooldown", type=int, default=COOLDOWN_SECS, help="seconds (default 900)")
    args = ap.parse_args()

    forced = args.force or os.environ.get("SBR_GPU_PREFLIGHT", "").lower() == "off"
    minutes = max(1, args.cooldown // 60)

    reasons: list[str] = []
    if (stamp := check_stamp()) is not None:
        reasons.append(stamp)

    hits, err = kernel_log(minutes)
    if err is not None:
        reasons.append(f"COULD NOT READ THE KERNEL LOG ({err}) — this is not a clean result, it is "
                       f"no result. Pass --force if you know the GPU is healthy.")
    elif hits:
        reasons.append(f"the kernel logged {len(hits)} amdgpu reset/timeout line(s) in the last "
                       f"{minutes} minutes; the most recent is:\n      {hits[-1].strip()}")

    if not reasons:
        print(f"[gpu-preflight] OK — no amdgpu reset or timeout in the last {minutes} minutes, "
              f"and no GPU-fault stamp from a previous run.")
        return 0

    head = "OVERRIDDEN" if forced else "REFUSING TO START"
    print(f"[gpu-preflight] {head}: the GPU is not in a state to take a render run.",
          file=sys.stderr)
    for r in reasons:
        print(f"  * {r}", file=sys.stderr)
    if forced:
        print("[gpu-preflight] --force given; starting anyway.", file=sys.stderr)
        return 0
    print("\n  This renderer has hung the graphics ring and taken the desktop session down with\n"
          "  it. A device that was just reset needs to settle, and the runs started straight\n"
          "  after a reset are the ones that escalated it. Wait out the cooldown, or pass\n"
          "  --force / SBR_GPU_PREFLIGHT=off if you know the card is healthy.\n", file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())

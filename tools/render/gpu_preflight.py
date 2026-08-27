#!/usr/bin/env python3
"""Refuse to start a GPU run when the device is in no state to take one.

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

So this is an INTERLOCK, not advice. `run.sh` and `run-render.sh` call it and exit if it says
no.

WHAT IT CHECKS
--------------
1. amdgpu illegal-register faults, ring timeouts / resets / "device wedged" in the kernel log
   within a cooldown window.
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
`--force` prints what it is overriding. Guarded launchers do not pass it.
"""

from __future__ import annotations

import argparse
import json
import sys
import tempfile
import time
from pathlib import Path

from gpu_events import atomic_durable_replace, current_boot_id, recent_kernel_faults

REPO = Path(__file__).resolve().parents[2]
STAMP = REPO / "scratch" / "gpu_fault.stamp"

# How long after a GPU reset this refuses to start a render run. Chosen from observation, not
# taste: after each reset in the 2026-08-12 session, runs started within a few minutes failed at
# SDL init ("XIO: fatal IO error 2") and the display session needed longer than that to settle.
COOLDOWN_SECS = 15 * 60


def check_stamp(
    cooldown_secs: int,
    stamp: Path = STAMP,
    now_monotonic_ns: int | None = None,
    boot_id: str | None = None,
) -> str | None:
    """Check a boot-id + CLOCK_MONOTONIC cooldown record without wall-clock mtime."""
    if cooldown_secs < 0:
        return f"negative GPU cooldown is invalid ({cooldown_secs}); refusing"
    if not stamp.exists():
        return None
    try:
        raw_stamp = stamp.read_text(errors="replace")
    except OSError as exc:
        return f"GPU fault stamp cannot be read ({exc}); refusing"
    try:
        payload = json.loads(raw_stamp)
        raw_boot_id = payload["boot_id"]
        if not isinstance(raw_boot_id, str) or not raw_boot_id or payload.get("boot_id_error"):
            raise ValueError("stamp has no trustworthy boot id")
        event_boot_id = raw_boot_id
        event_monotonic_ns = int(payload["monotonic_ns"])
        why = str(payload["reason"])
    except json.JSONDecodeError as exc:
        # Older guards wrote only a reason line.  Its wall-clock mtime is not a safe cooldown
        # clock, so migrate it once to the current boot/monotonic epoch and refuse for one full
        # cooldown. JSON-shaped damage is not a legacy format and remains fail-closed.
        if raw_stamp.strip() and not raw_stamp.lstrip().startswith(("{", "[")):
            if boot_id is None:
                boot_id, error = current_boot_id()
                if error is not None or boot_id is None:
                    return f"legacy GPU fault stamp exists but current boot id is unavailable ({error})"
            if now_monotonic_ns is None:
                now_monotonic_ns = time.clock_gettime_ns(time.CLOCK_MONOTONIC)
            original_reason = " ".join(raw_stamp.split())[:300]
            payload = {
                "version": 1,
                "boot_id": boot_id,
                "monotonic_ns": now_monotonic_ns,
                "reason": f"legacy stamp migrated; original reason: {original_reason}",
                "category": "legacy_migration",
            }
            try:
                atomic_durable_replace(stamp, json.dumps(payload, sort_keys=True) + "\n")
            except OSError as write_exc:
                return f"legacy GPU fault stamp cannot be migrated durably ({write_exc}); refusing"
            return ("legacy GPU fault stamp migrated to a current-boot monotonic record; "
                    "refusing for one full cooldown")
        return f"GPU fault stamp is not a trustworthy monotonic record ({exc}); refusing"
    except (KeyError, TypeError, ValueError) as exc:
        return f"GPU fault stamp is not a trustworthy monotonic record ({exc}); refusing"
    if boot_id is None:
        boot_id, error = current_boot_id()
        if error is not None or boot_id is None:
            return f"GPU fault stamp exists but current boot id is unavailable ({error})"
    if event_boot_id != boot_id:
        return None
    if now_monotonic_ns is None:
        now_monotonic_ns = time.clock_gettime_ns(time.CLOCK_MONOTONIC)
    age_ns = now_monotonic_ns - event_monotonic_ns
    if age_ns < 0:
        return ("GPU fault stamp is from the current boot but lies in the future on "
                f"CLOCK_MONOTONIC (age_ns={age_ns}); refusing")
    if age_ns > cooldown_secs * 1_000_000_000:
        return None
    age = age_ns // 1_000_000_000
    return (f"a guarded run recorded a GPU fault {age // 60}m{age % 60}s ago "
            f"(scratch/gpu_fault.stamp): {why[:300]}")


def preflight_reasons(cooldown_secs: int, stamp: Path = STAMP) -> list[str]:
    reasons: list[str] = []
    if (stamp_reason := check_stamp(cooldown_secs, stamp)) is not None:
        reasons.append(stamp_reason)
    hits, error = recent_kernel_faults(cooldown_secs)
    if error is not None:
        reasons.append(f"COULD NOT READ THE KERNEL LOG ({error}) — this is no result, not clean")
    elif hits:
        reasons.append(f"the kernel logged {len(hits)} GPU fault/reset line(s); the most recent "
                       f"is: {hits[-1].message.strip()}")
    return reasons


def selftest() -> int:
    (REPO / "scratch").mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="gpu-preflight-", dir=REPO / "scratch") as temp:
        stamp = Path(temp) / "stamp"
        payload = {"version": 1, "boot_id": "boot-a", "monotonic_ns": 5_000, "reason": "fault"}
        stamp.write_text(json.dumps(payload), encoding="utf-8")
        assert check_stamp(1, stamp, now_monotonic_ns=5_500, boot_id="boot-a") is not None
        assert check_stamp(1, stamp, now_monotonic_ns=4_999, boot_id="boot-a") is not None
        assert check_stamp(-1, stamp, now_monotonic_ns=5_500, boot_id="boot-a") is not None
        assert check_stamp(1, stamp, now_monotonic_ns=5_500, boot_id="boot-b") is None
        assert check_stamp(0, stamp, now_monotonic_ns=5_001, boot_id="boot-a") is None
        legacy = Path(temp) / "legacy-stamp"
        legacy.write_text("old DEVICE_LOST reason\n", encoding="utf-8")
        assert check_stamp(1, legacy, now_monotonic_ns=9_000, boot_id="boot-a") is not None
        migrated = json.loads(legacy.read_text(encoding="utf-8"))
        assert migrated["boot_id"] == "boot-a"
        assert migrated["monotonic_ns"] == 9_000
        assert migrated["category"] == "legacy_migration"
        assert "old DEVICE_LOST reason" in migrated["reason"]
        assert check_stamp(1, legacy, now_monotonic_ns=1_000_009_001,
                           boot_id="boot-a") is None
        malformed = Path(temp) / "malformed-stamp"
        malformed.write_text('{"version":', encoding="utf-8")
        assert "not a trustworthy" in check_stamp(1, malformed, boot_id="boot-a")
    print("gpu-preflight selftest PASS")
    print("  current/future/prior/expired and legacy-migration stamp controls pass")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--force", action="store_true", help="run anyway, printing what is overridden")
    ap.add_argument("--cooldown", type=int, default=COOLDOWN_SECS, help="seconds (default 900)")
    ap.add_argument("--selftest", action="store_true")
    args = ap.parse_args()
    if args.selftest:
        return selftest()
    if args.cooldown < 0:
        ap.error("--cooldown must be non-negative")

    forced = args.force
    minutes = max(1, args.cooldown // 60)

    reasons = preflight_reasons(args.cooldown)

    if not reasons:
        print(f"[gpu-preflight] OK — no GPU fault, reset or timeout in the last {minutes} minutes, "
              f"and no GPU-fault stamp from a previous run.")
        return 0

    head = "OVERRIDDEN" if forced else "REFUSING TO START"
    print(f"[gpu-preflight] {head}: the GPU is not in a state to take another run.",
          file=sys.stderr)
    for r in reasons:
        print(f"  * {r}", file=sys.stderr)
    if forced:
        print("[gpu-preflight] --force given; starting anyway.", file=sys.stderr)
        return 0
    print("\n  This renderer has hung the graphics ring and taken the desktop session down with\n"
          "  it. A device that was just reset needs to settle, and the runs started straight\n"
          "  after a reset are the ones that escalated it. Wait out the cooldown, or pass\n"
          "  --force if you know the card is healthy.\n", file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())

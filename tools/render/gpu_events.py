#!/usr/bin/env python3
"""Shared kernel-event definitions for Sunbright's GPU safety interlocks."""

from __future__ import annotations

import json
import os
import re
import subprocess
import tempfile
import time
from dataclasses import dataclass
from pathlib import Path


# The first signal in the 2026-08-26 incident did not contain the usual ``amdgpu:`` prefix:
# ``[drm:gfx_v10_0_priv_reg_irq [amdgpu]] *ERROR* Illegal register access ...``.  It must stop
# the run immediately; waiting for the later ring timeout gives the bad command stream more time
# to take the desktop down with it.
GPU_FAULT = re.compile(
    r"Illegal register access in command stream"
    r"|(?:amdgpu|\[drm:.*amdgpu).*"
    r"(?:ring .* timeout|ring .* reset (?:failed|succeeded)|GPU reset begin|GPUVM fault|"
    r"device wedged|\[(?:gfxhub|mmhub)\] page fault|PROTECTION_FAULT|VRAM is lost)",
    re.IGNORECASE,
)


@dataclass(frozen=True)
class KernelFault:
    message: str
    monotonic_us: int | None


def atomic_durable_replace(path: Path, text: str) -> None:
    """Atomically replace a small safety record and fsync its directory entry."""
    path.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary_name = tempfile.mkstemp(prefix=f".{path.name}.", dir=path.parent)
    temporary = Path(temporary_name)
    try:
        with os.fdopen(descriptor, "w", encoding="utf-8") as output:
            output.write(text)
            output.flush()
            os.fsync(output.fileno())
        os.replace(temporary, path)
        directory = os.open(path.parent, os.O_RDONLY | getattr(os, "O_DIRECTORY", 0))
        try:
            os.fsync(directory)
        finally:
            os.close(directory)
    except BaseException:
        try:
            temporary.unlink()
        except FileNotFoundError:
            pass
        raise


def is_gpu_fault(line: str) -> bool:
    """Return whether one kernel line is a fail-fast GPU fault signal."""
    return GPU_FAULT.search(line) is not None


def recent_kernel_faults(cooldown_secs: int) -> tuple[list[KernelFault], str | None]:
    """Read current-boot GPU faults inside a monotonic cooldown window.

    Kernel monotonic timestamps remain ordered when the wall clock steps backward.  That happened
    on this machine and made ``journalctl --since`` an unsafe cooldown boundary.
    """
    try:
        result = subprocess.run(
            ["journalctl", "-k", "-b", "--no-pager", "-o", "json"],
            capture_output=True,
            text=True,
            timeout=30,
        )
    except FileNotFoundError:
        return [], "journalctl is not installed"
    except subprocess.TimeoutExpired:
        return [], "journalctl timed out"
    if result.returncode != 0:
        return [], f"journalctl exited {result.returncode}: {result.stderr.strip()[:200]}"

    # journal __MONOTONIC_TIMESTAMP uses CLOCK_MONOTONIC, not CLOCK_BOOTTIME. Mixing the two
    # clocks ages every event by accumulated suspend time and can expire a cooldown early.
    now_us = time.clock_gettime_ns(time.CLOCK_MONOTONIC) // 1_000
    cutoff_us = now_us - max(0, cooldown_secs) * 1_000_000
    faults: list[KernelFault] = []
    for raw in result.stdout.splitlines():
        if not raw.strip():
            continue
        try:
            record = json.loads(raw)
        except json.JSONDecodeError as exc:
            return [], f"journalctl returned malformed JSON: {exc}"
        message = str(record.get("MESSAGE", ""))
        if not is_gpu_fault(message):
            continue
        try:
            monotonic_us = int(record["__MONOTONIC_TIMESTAMP"])
        except (KeyError, TypeError, ValueError):
            # An un-timestamped fault is uncertain, not clean. Keep it so preflight refuses.
            monotonic_us = None
        if monotonic_us is None or monotonic_us >= cutoff_us:
            faults.append(KernelFault(message=message, monotonic_us=monotonic_us))
    return faults, None


def current_kernel_cursor() -> tuple[str | None, str | None]:
    """Return a cursor at the current end of the current boot's kernel journal."""
    try:
        result = subprocess.run(
            ["journalctl", "-k", "-b", "-n", "0", "--show-cursor", "--no-pager"],
            capture_output=True,
            text=True,
            timeout=10,
        )
    except FileNotFoundError:
        return None, "journalctl is not installed"
    except subprocess.TimeoutExpired:
        return None, "journalctl cursor query timed out"
    if result.returncode != 0:
        return None, f"journalctl exited {result.returncode}: {result.stderr.strip()[:200]}"
    for line in reversed(result.stdout.splitlines()):
        if line.startswith("-- cursor: "):
            return line.removeprefix("-- cursor: ").strip(), None
    return None, "journalctl did not return a kernel cursor"


def kernel_lines_after_cursor(cursor: str) -> tuple[list[str], str | None]:
    """Return all current-boot kernel lines appended after an exact journal cursor."""
    try:
        result = subprocess.run(
            [
                "journalctl", "-k", "-b", "--after-cursor", cursor,
                "--no-pager", "-o", "short-iso-precise",
            ],
            capture_output=True,
            text=True,
            timeout=10,
        )
    except FileNotFoundError:
        return [], "journalctl is not installed"
    except subprocess.TimeoutExpired:
        return [], "journalctl cursor scan timed out"
    if result.returncode != 0:
        return [], f"journalctl exited {result.returncode}: {result.stderr.strip()[:200]}"
    return result.stdout.splitlines(), None


def current_boot_id() -> tuple[str | None, str | None]:
    try:
        with open("/proc/sys/kernel/random/boot_id", encoding="ascii") as source:
            boot_id = source.read().strip()
    except OSError as exc:
        return None, f"cannot read boot id: {exc}"
    return (boot_id, None) if boot_id else (None, "kernel returned an empty boot id")

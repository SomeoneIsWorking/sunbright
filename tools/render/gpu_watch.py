#!/usr/bin/env python3
"""Run one command while watching the kernel journal for the first new GPU fault.

The watched command starts in a new process group. On the first fault this tool immediately
SIGKILLs that exact process group, then writes a crash-surviving incident bundle and GPU-fault
cooldown stamp and decodes the matching recomp submit-flight file when the reader is available.
"""

from __future__ import annotations

import argparse
import collections
import json
import math
import os
import platform
import queue
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import threading
import time
from unittest import mock
from dataclasses import dataclass
from datetime import datetime, timedelta, timezone
from pathlib import Path

from gpu_events import (
    atomic_durable_replace,
    current_boot_id,
    current_kernel_cursor,
    is_gpu_fault,
    kernel_lines_after_cursor,
)
from gpu_preflight import COOLDOWN_SECS, preflight_reasons


REPO = Path(__file__).resolve().parents[2]
DEFAULT_INCIDENT_DIR = REPO / "scratch" / "gpu_crash"
DEFAULT_STAMP = REPO / "scratch" / "gpu_fault.stamp"
WATCH_FAULT_RC = 86
WATCH_BROKEN_RC = 85
OUTPUT_TAIL_LINES = 160
POST_EXIT_SETTLE_SECS = 0.4
REAP_TIMEOUT_SECS = 1.0
MAX_GUARD_TIMEOUT_SECS = 600.0
KERNEL_TIMESTAMP = re.compile(
    r"^(?P<second>\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2})"
    r"(?:\.(?P<fraction>\d{1,9}))?"
    r"(?P<zone>Z|[+-]\d{2}:?\d{2})(?:\s|$)"
)
SECRET_NAME = re.compile(r"(?:TOKEN|KEY|PASS(?:WORD)?|AUTH|SECRET)", re.IGNORECASE)


@dataclass
class GuardResult:
    returncode: int
    fault_line: str | None = None
    incident_path: Path | None = None


class OutputPump(threading.Thread):
    def __init__(self, source, output_log: Path | None):
        super().__init__(daemon=True)
        self.source = source
        self.output_log = output_log
        self.tail: collections.deque[str] = collections.deque(maxlen=OUTPUT_TAIL_LINES)
        self.tail_lock = threading.Lock()

    def snapshot(self) -> list[str]:
        with self.tail_lock:
            return list(self.tail)

    def run(self) -> None:
        log = None
        try:
            if self.output_log is not None:
                self.output_log.parent.mkdir(parents=True, exist_ok=True)
                log = self.output_log.open("w", encoding="utf-8", errors="replace")
            for line in self.source:
                with self.tail_lock:
                    self.tail.append(line.rstrip("\n"))
                sys.stdout.write(line)
                sys.stdout.flush()
                if log is not None:
                    log.write(line)
                    log.flush()
        finally:
            if log is not None:
                log.close()


class JournalPump(threading.Thread):
    def __init__(self, source):
        super().__init__(daemon=True)
        self.source = source
        self.lines: queue.Queue[str] = queue.Queue()
        self.eof = threading.Event()

    def run(self) -> None:
        try:
            for line in self.source:
                self.lines.put(line.rstrip())
        finally:
            self.eof.set()


class ErrorPump(threading.Thread):
    def __init__(self, source):
        super().__init__(daemon=True)
        self.source = source
        self.lines: collections.deque[str] = collections.deque(maxlen=20)

    def run(self) -> None:
        for line in self.source:
            self.lines.append(line.rstrip())


class SignalProtection:
    """Keep owned process cleanup reachable for the guard's entire owned lifetime."""

    handled = (signal.SIGINT, signal.SIGTERM, signal.SIGHUP)

    def __init__(self) -> None:
        self.received = 0
        self.pgid: int | None = None
        self.journal: subprocess.Popen[str] | None = None
        self._old_handlers: dict[int, object] = {}
        self._prior_mask = None

    def _on_signal(self, signum, _frame) -> None:
        self.received = signum
        if self.pgid is not None:
            _kill_exact_group(self.pgid)

    def __enter__(self) -> SignalProtection:
        if threading.current_thread() is not threading.main_thread():
            raise RuntimeError("GPU guard must run on the main thread for signal-safe cleanup")
        self._prior_mask = signal.pthread_sigmask(signal.SIG_BLOCK, self.handled)
        try:
            for handled in self.handled:
                self._old_handlers[handled] = signal.getsignal(handled)
                signal.signal(handled, self._on_signal)
        finally:
            signal.pthread_sigmask(signal.SIG_SETMASK, self._prior_mask)
        return self

    def popen_journal(self, command: list[str]) -> subprocess.Popen[str]:
        current_mask = signal.pthread_sigmask(signal.SIG_BLOCK, self.handled)
        try:
            process = subprocess.Popen(
                command, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                text=True, bufsize=1,
            )
            self.journal = process
            return process
        finally:
            signal.pthread_sigmask(signal.SIG_SETMASK, current_mask)

    def popen_child(self, command: list[str]) -> subprocess.Popen[str]:
        current_mask = signal.pthread_sigmask(signal.SIG_BLOCK, self.handled)
        try:
            process = subprocess.Popen(
                command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                text=True, errors="replace", bufsize=1, start_new_session=True,
            )
            self.pgid = process.pid
            return process
        finally:
            signal.pthread_sigmask(signal.SIG_SETMASK, current_mask)

    def __exit__(self, _exc_type, _exc, _traceback) -> None:
        # This is deliberately the last operation after the caller has killed/reaped every owned
        # process and joined every pump. A signal before here still reaches _on_signal.
        signal.pthread_sigmask(signal.SIG_BLOCK, self.handled)
        try:
            for handled, previous in self._old_handlers.items():
                signal.signal(handled, previous)
        finally:
            signal.pthread_sigmask(signal.SIG_SETMASK, self._prior_mask)


def _kill_exact_group(pgid: int) -> None:
    """Kill the immutable guarded PGID even when its original leader already exited."""
    try:
        os.killpg(pgid, signal.SIGKILL)
    except ProcessLookupError:
        pass


def _bounded_reap(process: subprocess.Popen[str], timeout: float = REAP_TIMEOUT_SECS) -> str | None:
    try:
        process.wait(timeout=timeout)
    except subprocess.TimeoutExpired:
        return f"pid {process.pid} did not reap within {timeout:g}s after SIGKILL"
    return None


def _stop_journal(journal: subprocess.Popen[str]) -> None:
    if journal.poll() is None:
        journal.terminate()
    try:
        journal.wait(timeout=REAP_TIMEOUT_SECS)
    except subprocess.TimeoutExpired:
        journal.kill()
        note = _bounded_reap(journal)
        if note:
            print(f"[gpu-watch] journal {note}", file=sys.stderr)


def _kernel_context() -> str:
    try:
        result = subprocess.run(
            ["journalctl", "-k", "-b", "-n", "80", "--no-pager"],
            capture_output=True,
            text=True,
            timeout=10,
        )
    except (FileNotFoundError, subprocess.TimeoutExpired) as exc:
        return f"kernel context unavailable: {exc}"
    if result.returncode != 0:
        return f"kernel context unavailable: journalctl exited {result.returncode}"
    return result.stdout.rstrip()


def kernel_realtime_ns(line: str) -> int | None:
    """Parse journalctl short-iso-precise time without a float/microsecond round trip."""
    match = KERNEL_TIMESTAMP.match(line)
    if match is None:
        return None
    fraction_ns = int((match.group("fraction") or "").ljust(9, "0"))
    zone_text = match.group("zone")
    if zone_text == "Z":
        zone = timezone.utc
    else:
        compact = zone_text.replace(":", "")
        sign = 1 if compact[0] == "+" else -1
        offset_minutes = sign * (int(compact[1:3]) * 60 + int(compact[3:5]))
        zone = timezone(timedelta(minutes=offset_minutes))
    local_second = datetime.strptime(match.group("second"), "%Y-%m-%dT%H:%M:%S").replace(
        tzinfo=zone
    )
    utc_second = local_second.astimezone(timezone.utc).replace(tzinfo=None)
    epoch = datetime(1970, 1, 1)
    delta = utc_second - epoch
    seconds = delta.days * 86_400 + delta.seconds
    return seconds * 1_000_000_000 + fraction_ns


def _flight_analysis(
    child_pid: int,
    kernel_real_ns: int | None,
    flight_snapshot: dict[str, tuple[int, int, int]],
    incident_dir: Path = DEFAULT_INCIDENT_DIR,
    reader: Path | None = None,
) -> str:
    flight_files = sorted(
        (
            path for path in incident_dir.glob(f"session_{child_pid}_*.flight")
            if flight_snapshot.get(str(path.resolve())) != (
                path.stat().st_ino, path.stat().st_size, path.stat().st_mtime_ns
            )
        ),
        key=lambda path: path.stat().st_mtime_ns,
    )
    if not flight_files:
        return "no new/changed post-launch submit-flight file matched the guarded process PID"
    reader = reader or (REPO / "build-sms-recomp" / "gpu_flight_dump")
    newest = flight_files[-1]
    if not reader.is_file() or not os.access(reader, os.X_OK):
        return f"submit-flight file: {newest} (reader is unavailable)"
    arguments = [str(reader), str(newest), "--tail", "24"]
    if kernel_real_ns is not None:
        arguments.extend(("--kernel-real-ns", str(kernel_real_ns)))
    try:
        result = subprocess.run(
            arguments,
            capture_output=True,
            text=True,
            timeout=15,
        )
    except (OSError, subprocess.TimeoutExpired) as exc:
        return f"gpu_flight_dump failed safely: {exc}"
    text = (result.stdout + result.stderr).rstrip()
    if result.returncode != 0:
        return f"gpu_flight_dump exited {result.returncode}: {text or '<no output>'}"
    filename = re.match(rf"session_{child_pid}_([0-9a-fA-F]+)_.+\.flight$", newest.name)
    if filename is None:
        return f"reader output rejected: flight filename has no session token: {newest.name}"
    session_token = filename.group(1).lower().lstrip("0") or "0"
    expected_pid = f"pid={child_pid}"
    expected_session = f"session={session_token}"
    lowered = text.lower()
    if expected_pid not in text or expected_session not in lowered:
        return ("reader output rejected: flight identity does not match guarded launch "
                f"({expected_pid}, {expected_session})\n{text}")
    return text or f"gpu_flight_dump exited {result.returncode} without output"


def _redacted_command(command: list[str]) -> list[str]:
    redacted: list[str] = []
    for argument in command:
        name, separator, _value = argument.partition("=")
        redacted.append(f"{name}=<redacted>" if separator and SECRET_NAME.search(name) else argument)
    return redacted


def _static_environment(command: list[str]) -> list[str]:
    """Return CPU-only host/process facts; never enumerate or initialize a GPU API."""
    uname = platform.uname()
    lines = [
        f"kernel: {uname.system} {uname.release} {uname.machine}",
        f"guarded command: {_redacted_command(command)!r}",
    ]
    for device in sorted(Path("/sys/class/drm").glob("card*/device")):
        try:
            driver = device.resolve().joinpath("driver").resolve().name
            vendor = device.joinpath("vendor").read_text().strip()
            product = device.joinpath("device").read_text().strip()
            revision_path = device / "revision"
            revision = revision_path.read_text().strip() if revision_path.exists() else "unknown"
        except OSError:
            continue
        lines.append(
            f"drm {device.parent.name}: driver={driver} pci={vendor}:{product} revision={revision}"
        )
    if shutil.which("rpm"):
        try:
            result = subprocess.run(
                ["rpm", "-q", "mesa-vulkan-drivers", "vulkan-loader"],
                capture_output=True,
                text=True,
                timeout=5,
            )
        except (OSError, subprocess.TimeoutExpired) as exc:
            lines.append(f"package versions unavailable: {exc}")
        else:
            package_lines = (result.stdout + result.stderr).strip().splitlines()
            lines.extend(f"package: {line}" for line in package_lines)
    relevant = {}
    for key, value in os.environ.items():
        if not key.startswith(("SB_", "SBR_", "LUCENT_")):
            continue
        relevant[key] = "<redacted>" if SECRET_NAME.search(key) else value
    lines.extend(f"env {key}={value}" for key, value in sorted(relevant.items()))
    return lines


def _write_minimal_fault(
    incident_dir: Path,
    stamp: Path,
    pgid: int,
    reason: str,
    kernel_real_ns: int | None,
    category: str,
) -> Path:
    incident_dir.mkdir(parents=True, exist_ok=True)
    timestamp = datetime.now(timezone.utc)
    incident = incident_dir / f"watch_{timestamp.strftime('%Y%m%dT%H%M%S.%fZ')}_{pgid}.txt"
    minimal = (
        "Sunbright live GPU-fault guard STOP RECORD\n"
        f"utc: {timestamp.isoformat()}\n"
        f"guarded pid/pgid: {pgid}\n"
        f"STOP CATEGORY: {category}\n"
        f"FIRST STOP EVIDENCE: {reason}\n"
        f"first kernel real_ns: {kernel_real_ns if kernel_real_ns is not None else 'UNAVAILABLE'}\n"
        "state: process-group SIGKILL sent; minimal stop record persisted; enrichment pending\n"
    )
    atomic_durable_replace(incident, minimal)
    boot_id, boot_error = current_boot_id()
    stamp_payload = {
        "version": 1,
        "boot_id": boot_id,
        "monotonic_ns": time.clock_gettime_ns(time.CLOCK_MONOTONIC),
        "reason": reason,
        "category": category,
        "incident": str(incident),
    }
    if boot_error is not None:
        stamp_payload["boot_id_error"] = boot_error
    atomic_durable_replace(stamp, json.dumps(stamp_payload, sort_keys=True) + "\n")
    return incident


def _write_incident(
    incident_dir: Path,
    incident: Path,
    child_pid: int,
    command: list[str],
    reason: str,
    output_tail: list[str],
    include_flight: bool,
    flight_snapshot: dict[str, tuple[int, int, int]],
    include_context: bool = True,
    include_static: bool = True,
    live_kernel_lines: list[str] | None = None,
    kernel_real_ns: int | None = None,
    flight_reader: Path | None = None,
    reap_note: str | None = None,
    category: str = "gpu_fault",
) -> Path:
    timestamp = datetime.now(timezone.utc)
    sections = [
        "Sunbright live GPU-fault guard incident",
        f"utc: {timestamp.isoformat()}",
        f"guarded pid/pgid: {child_pid}",
        f"command: {_redacted_command(command)!r}",
        f"STOP CATEGORY: {category}",
        f"FIRST STOP EVIDENCE: {reason}",
        (f"first kernel real_ns: {kernel_real_ns}" if kernel_real_ns is not None
         else "first kernel real_ns: UNAVAILABLE (timestamp did not parse)"),
        "",
        "--- guarded process output tail ---",
        *(output_tail or ["<no process output captured>"]),
    ]
    if reap_note is not None:
        sections.extend(("", f"REAP STATUS: {reap_note}"))
    if live_kernel_lines:
        sections.extend((
            "",
            "--- live kernel lines from the causal anchor ---",
            *live_kernel_lines,
        ))
    if include_static:
        sections.extend(("", "--- static CPU-only environment ---", *_static_environment(command)))
    if include_context:
        sections.extend(("", "--- current kernel tail ---", _kernel_context()))
    if include_flight and category == "gpu_fault":
        sections.extend((
            "",
            "--- matching submit-flight analysis ---",
            _flight_analysis(
                child_pid, kernel_real_ns, flight_snapshot, incident_dir, flight_reader
            ),
        ))
    atomic_durable_replace(incident, "\n".join(sections) + "\n")
    return incident


def _collect_fault_context(
    journal_pump: JournalPump, first_line: str
) -> list[str]:
    """Collect the ring/process attribution immediately following the first fault.

    The guarded process is already dead when this runs. The short bounded window exists only to
    retain the kernel's lines that name the process and ring; it never delays stopping GPU work.
    """
    lines = [first_line]
    found_ring = "ring " in first_line.lower()
    found_process = " process " in first_line.lower()
    deadline = time.monotonic() + 2.0
    while time.monotonic() < deadline and not (found_ring and found_process):
        try:
            clean = journal_pump.lines.get(timeout=0.1)
        except queue.Empty:
            continue
        lines.append(clean)
        lowered = clean.lower()
        found_ring = found_ring or "ring " in lowered
        found_process = found_process or " process " in lowered
    return lines


def _journal_command(cursor: str) -> list[str]:
    return [
        "journalctl",
        "-k",
        "-b",
        "-f",
        "--after-cursor",
        cursor,
        "--no-pager",
        "-o",
        "short-iso-precise",
    ]


def run_guarded(
    command: list[str],
    timeout_secs: float,
    output_log: Path | None = None,
    incident_dir: Path = DEFAULT_INCIDENT_DIR,
    stamp: Path = DEFAULT_STAMP,
    kernel_command: list[str] | None = None,
    include_flight: bool = True,
) -> GuardResult:
    with SignalProtection() as signal_protection:
        return _run_guarded_protected(
            command, timeout_secs, output_log, incident_dir, stamp,
            kernel_command, include_flight, signal_protection,
        )


def _run_guarded_protected(
    command: list[str],
    timeout_secs: float,
    output_log: Path | None,
    incident_dir: Path,
    stamp: Path,
    kernel_command: list[str] | None,
    include_flight: bool,
    signal_protection: SignalProtection,
) -> GuardResult:
    start_cursor: str | None = None
    if kernel_command is None:
        start_cursor, error = current_kernel_cursor()
        if error is not None or start_cursor is None:
            print(f"[gpu-watch] REFUSING: cannot establish kernel cursor: {error}", file=sys.stderr)
            return GuardResult(WATCH_BROKEN_RC)
        reasons = preflight_reasons(COOLDOWN_SECS, stamp)
        gap_lines, gap_error = kernel_lines_after_cursor(start_cursor)
        if gap_error is not None:
            reasons.append(f"could not inspect preflight/cursor gap: {gap_error}")
        reasons.extend(
            f"GPU fault appeared during preflight: {line}" for line in gap_lines
            if is_gpu_fault(line)
        )
        if reasons:
            print("[gpu-watch] REFUSING BEFORE LAUNCH:", file=sys.stderr)
            for reason in reasons:
                print(f"  * {reason}", file=sys.stderr)
            return GuardResult(WATCH_BROKEN_RC)
        kernel_command = _journal_command(start_cursor)
    if signal_protection.received:
        return GuardResult(128 + signal_protection.received)

    try:
        journal = signal_protection.popen_journal(kernel_command)
    except OSError as exc:
        print(f"[gpu-watch] REFUSING: cannot start kernel watcher: {exc}", file=sys.stderr)
        return GuardResult(WATCH_BROKEN_RC)

    child: subprocess.Popen[str] | None = None
    pump: OutputPump | None = None
    assert journal.stdout is not None and journal.stderr is not None
    journal_pump = JournalPump(journal.stdout)
    error_pump = ErrorPump(journal.stderr)
    journal_pump.start()
    error_pump.start()
    if signal_protection.received:
        _stop_journal(journal)
        journal_pump.join(timeout=REAP_TIMEOUT_SECS)
        error_pump.join(timeout=REAP_TIMEOUT_SECS)
        return GuardResult(128 + signal_protection.received)
    if start_cursor is not None:
        # The follower is now alive. Scan from the original anchor once more before creating the
        # child; anything appended after this scan remains covered by that same follower cursor.
        barrier_lines, barrier_error = kernel_lines_after_cursor(start_cursor)
        if barrier_error is not None or any(is_gpu_fault(line) for line in barrier_lines):
            _stop_journal(journal)
            journal_pump.join(timeout=REAP_TIMEOUT_SECS)
            error_pump.join(timeout=REAP_TIMEOUT_SECS)
            reason = barrier_error or next(line for line in barrier_lines if is_gpu_fault(line))
            print(f"[gpu-watch] REFUSING AT PRE-LAUNCH BARRIER: {reason}", file=sys.stderr)
            return GuardResult(WATCH_BROKEN_RC, reason)

    pgid: int | None = None
    flight_snapshot = {
        str(path.resolve()): (path.stat().st_ino, path.stat().st_size, path.stat().st_mtime_ns)
        for path in incident_dir.glob("session_*.flight")
    }
    try:
        if signal_protection.received:
            return GuardResult(128 + signal_protection.received)
        child = signal_protection.popen_child(command)
        pgid = child.pid
        assert child.stdout is not None
        pump = OutputPump(child.stdout, output_log)
        pump.start()
        deadline = time.monotonic() + timeout_secs
        leader_exit_deadline: float | None = None

        def fault_result(reason: str, code: int, category: str = "gpu_fault") -> GuardResult:
            # Stopping submissions is the first action at the fault boundary.
            _kill_exact_group(pgid)
            kernel_ns = kernel_realtime_ns(reason)
            incident: Path | None = None
            persist_error: str | None = None
            try:
                incident = _write_minimal_fault(
                    incident_dir, stamp, pgid, reason, kernel_ns, category
                )
            except Exception as exc:
                persist_error = f"minimal incident persistence failed: {exc}"
            reap_note = _bounded_reap(child) or "leader reaped within the bounded wait"
            pump.join(timeout=0.25)
            live_kernel_lines = (
                _collect_fault_context(journal_pump, reason)
                if category == "gpu_fault" else [reason]
            )
            if persist_error is not None:
                print(f"[gpu-watch] {persist_error}", file=sys.stderr)
            if incident is not None:
                try:
                    _write_incident(
                        incident_dir, incident, child.pid, command, reason,
                        pump.snapshot(), include_flight=include_flight,
                        flight_snapshot=flight_snapshot,
                        live_kernel_lines=live_kernel_lines, kernel_real_ns=kernel_ns,
                        reap_note=reap_note, category=category,
                    )
                except Exception as exc:
                    print(f"[gpu-watch] enrichment failed; minimal incident survives: {exc}",
                          file=sys.stderr)
            label = str(incident) if incident is not None else "UNAVAILABLE"
            print(f"[gpu-watch] stopped exact pid/pgid {pgid}; incident: {label}",
                  file=sys.stderr)
            return GuardResult(code, reason, incident)

        while True:
            if signal_protection.received:
                _kill_exact_group(pgid)
                note = _bounded_reap(child)
                if note:
                    print(f"[gpu-watch] {note}", file=sys.stderr)
                return GuardResult(128 + signal_protection.received)
            if time.monotonic() >= deadline:
                print(f"[gpu-watch] wall-clock cap {timeout_secs:g}s reached; killing pid/pgid "
                      f"{pgid}.", file=sys.stderr)
                _kill_exact_group(pgid)
                note = _bounded_reap(child)
                if note:
                    print(f"[gpu-watch] {note}", file=sys.stderr)
                return GuardResult(124)

            try:
                line = journal_pump.lines.get(timeout=0.1)
            except queue.Empty:
                line = None
            if line is not None and is_gpu_fault(line):
                return fault_result(line, WATCH_FAULT_RC)

            if journal.poll() is not None and journal_pump.eof.is_set() and journal_pump.lines.empty():
                details = "; ".join(error_pump.lines) or "no diagnostic output"
                reason = f"kernel watcher exited {journal.returncode}: {details}"
                return fault_result(reason, WATCH_BROKEN_RC, "monitor_failure")

            leader_rc = child.poll()
            if leader_rc is not None:
                if leader_exit_deadline is None:
                    leader_exit_deadline = time.monotonic() + POST_EXIT_SETTLE_SECS
                if time.monotonic() >= leader_exit_deadline and journal_pump.lines.empty():
                    if start_cursor is not None:
                        final_lines, final_error = kernel_lines_after_cursor(start_cursor)
                        if final_error is not None:
                            return fault_result(
                                f"post-exit kernel barrier failed: {final_error}",
                                WATCH_BROKEN_RC, "monitor_failure",
                            )
                        first_fault = next(
                            (candidate for candidate in final_lines if is_gpu_fault(candidate)), None
                        )
                        if first_fault is not None:
                            return fault_result(first_fault, WATCH_FAULT_RC)
                    # The leader may have exited while descendants retained the process group.
                    _kill_exact_group(pgid)
                    return GuardResult(leader_rc)
    finally:
        if pgid is not None:
            _kill_exact_group(pgid)
        if child is not None:
            note = _bounded_reap(child)
            if note:
                print(f"[gpu-watch] {note}", file=sys.stderr)
        _stop_journal(journal)
        if pump is not None:
            pump.join(timeout=2)
        journal_pump.join(timeout=2)
        error_pump.join(timeout=2)


def _process_stopped(pid: int) -> bool:
    status = Path(f"/proc/{pid}/status")
    if not status.exists():
        return True
    for line in status.read_text(errors="replace").splitlines():
        if line.startswith("State:"):
            return "Z" in line
    return False


def _wait_for_path(path: Path, timeout: float = 3.0) -> None:
    deadline = time.monotonic() + timeout
    while not path.exists() and time.monotonic() < deadline:
        time.sleep(0.01)
    if not path.exists():
        raise AssertionError(f"timed out waiting for selftest marker {path}")


def _wait_process_stopped(pid: int, timeout: float = 1.0) -> None:
    """Allow SIGKILL delivery/reparenting to settle before judging the process-tree control."""
    deadline = time.monotonic() + timeout
    while not _process_stopped(pid) and time.monotonic() < deadline:
        time.sleep(0.01)
    if not _process_stopped(pid):
        raise AssertionError(f"guarded descendant {pid} survived process-group SIGKILL")


def selftest() -> int:
    module = sys.modules[__name__]
    planted = ("2026-08-26T22:48:34.123456789+03:00 kernel: "
               "[drm:gfx_v10_0_priv_reg_irq [amdgpu]] *ERROR* "
               "Illegal register access in command stream")
    planted_feed = (
        planted + "\n"
        "2026-08-26T22:48:34.123456790+03:00 kernel: amdgpu: ring gfx_0.0.0 timeout\n"
        "2026-08-26T22:48:34.123456791+03:00 kernel: amdgpu: Process planted pid 123\n"
    )
    expected_real_ns = 1_787_773_714_123_456_789
    assert kernel_realtime_ns(planted) == expected_real_ns
    assert kernel_realtime_ns(planted.replace("+03:00", "+0300")) == expected_real_ns
    assert is_gpu_fault(planted)
    assert is_gpu_fault("amdgpu: ring gfx_0.0.0 timeout, emitted seq=2")
    assert not is_gpu_fault("amdgpu: ring gfx_0.0.0 uses VM inv eng 0 on hub 0")

    (REPO / "scratch").mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(
        prefix="gpu-watch-selftest-", dir=REPO / "scratch"
    ) as temp_text:
        temp = Path(temp_text)

        clean_fifo = temp / "clean.fifo"
        os.mkfifo(clean_fifo)
        clean_kernel = [
            sys.executable, "-u", "-c",
            f"import time; print(open({str(clean_fifo)!r}).read(), end=''); time.sleep(30)",
        ]
        clean_child = [sys.executable, "-u", "-c",
                       f"open({str(clean_fifo)!r}, 'w').write('kernel: harmless control line\\n')"]
        clean = run_guarded(
            clean_child, 5, incident_dir=temp / "incidents", stamp=temp / "stamp",
            kernel_command=clean_kernel, include_flight=False,
        )
        assert clean.returncode == 0, clean
        assert not (temp / "stamp").exists()

        fault_fifo = temp / "fault.fifo"
        os.mkfifo(fault_fifo)
        child_marker = temp / "child.pid"
        fault_kernel = [
            sys.executable, "-u", "-c",
            f"import time; print(open({str(fault_fifo)!r}).read(), end=''); time.sleep(30)",
        ]
        fault_script = (
            "import subprocess,sys,time; "
            "p=subprocess.Popen([sys.executable,'-c','import time; time.sleep(30)']); "
            f"open({str(child_marker)!r},'w').write(str(p.pid)); "
            f"open({str(fault_fifo)!r},'w').write({planted_feed!r}); "
            "print('planted-fault child still alive', flush=True); time.sleep(30)"
        )
        stop_order: list[str] = []
        real_kill = _kill_exact_group
        real_minimal = _write_minimal_fault

        def ordered_kill(pgid: int) -> None:
            stop_order.append("kill")
            real_kill(pgid)

        def ordered_minimal(*args, **kwargs):
            stop_order.append("persist")
            return real_minimal(*args, **kwargs)

        with (
            mock.patch.object(module, "_kill_exact_group", side_effect=ordered_kill),
            mock.patch.object(module, "_write_minimal_fault", side_effect=ordered_minimal),
        ):
            fault = run_guarded(
                [sys.executable, "-u", "-c", fault_script], 5,
                incident_dir=temp / "incidents", stamp=temp / "stamp",
                kernel_command=fault_kernel, include_flight=False,
            )
        assert fault.returncode == WATCH_FAULT_RC, fault
        assert stop_order.index("kill") < stop_order.index("persist")
        assert fault.incident_path is not None and fault.incident_path.is_file()
        incident_text = fault.incident_path.read_text(errors="replace")
        assert "Illegal register access" in incident_text
        assert "2026-08-26T22:48:34.123456789+03:00" in incident_text
        assert f"first kernel real_ns: {expected_real_ns}" in incident_text
        assert "ring gfx_0.0.0 timeout" in incident_text
        assert "Process planted pid 123" in incident_text
        assert "STOP CATEGORY: gpu_fault" in incident_text
        stamp_payload = json.loads((temp / "stamp").read_text())
        assert stamp_payload["version"] == 1
        assert stamp_payload["category"] == "gpu_fault"
        assert isinstance(stamp_payload["monotonic_ns"], int)
        grandchild_pid = int(child_marker.read_text())
        _wait_process_stopped(grandchild_pid)

        flight_dir = temp / "flight"
        flight_dir.mkdir()
        fake_flight = flight_dir / "session_4242_abcd_control.flight"
        fake_flight.write_bytes(b"control")
        fake_reader = temp / "fake_flight_dump.py"
        fake_reader.write_text(
            f"#!{sys.executable}\n"
            "import sys\n"
            f"expected = {str(expected_real_ns)!r}\n"
            "i = sys.argv.index('--kernel-real-ns')\n"
            "assert sys.argv[i + 1] == expected\n"
            "print(\"session     : label='control' pid=4242 session=abcd\")\n"
            "print('CAUSAL-WINDOW CANDIDATE fake-reader received ' + expected)\n",
            encoding="utf-8",
        )
        fake_reader.chmod(0o755)
        flight_incident = _write_minimal_fault(
            flight_dir, temp / "flight.stamp", 4242, planted, expected_real_ns, "gpu_fault"
        )
        _write_incident(
            flight_dir,
            flight_incident,
            4242,
            ["fake-command"],
            planted,
            [],
            include_flight=True,
            flight_snapshot={},
            include_context=False,
            include_static=False,
            kernel_real_ns=expected_real_ns,
            flight_reader=fake_reader,
            category="gpu_fault",
        )
        flight_text = flight_incident.read_text(errors="replace")
        assert "CAUSAL-WINDOW CANDIDATE fake-reader" in flight_text
        assert str(expected_real_ns) in flight_text

        durable_control = temp / "durable" / "control.txt"
        with mock.patch.object(os, "fsync", wraps=os.fsync) as fsync_control:
            atomic_durable_replace(durable_control, "durable-control\n")
        assert fsync_control.call_count >= 2, "file and new directory entry were not both fsynced"
        assert durable_control.read_text() == "durable-control\n"
        with mock.patch.object(os, "replace", side_effect=OSError("planted replace failure")):
            try:
                atomic_durable_replace(durable_control, "must-not-truncate\n")
            except OSError:
                pass
            else:
                raise AssertionError("planted atomic replacement failure was swallowed")
        assert durable_control.read_text() == "durable-control\n"

        exact_snapshot = {
            str(fake_flight.resolve()): (
                fake_flight.stat().st_ino, fake_flight.stat().st_size,
                fake_flight.stat().st_mtime_ns,
            )
        }
        assert "no new/changed" in _flight_analysis(
            4242, expected_real_ns, exact_snapshot, flight_dir, fake_reader
        )
        with mock.patch.object(
            subprocess, "run", side_effect=subprocess.TimeoutExpired(["fake-reader"], 15)
        ):
            assert "failed safely" in _flight_analysis(
                4242, expected_real_ns, {}, flight_dir, fake_reader
            )

        with mock.patch.dict(os.environ, {"SBR_AUTH_TOKEN": "must-not-leak"}, clear=True):
            static = "\n".join(_static_environment([
                "control", "SBR_AUTH_TOKEN=argv-must-not-leak", "--password=also-secret",
            ]))
        assert "env SBR_AUTH_TOKEN=<redacted>" in static
        assert "must-not-leak" not in static
        assert "argv-must-not-leak" not in static
        assert "also-secret" not in static
        assert "SBR_AUTH_TOKEN=<redacted>" in static
        assert "--password=<redacted>" in static

        with (
            mock.patch.object(module, "current_kernel_cursor", return_value=("anchor", None)),
            mock.patch.object(module, "preflight_reasons", return_value=[]),
            mock.patch.object(module, "kernel_lines_after_cursor", return_value=([planted], None)),
            mock.patch.object(subprocess, "Popen") as forbidden_launch,
        ):
            gap = run_guarded(["must-not-launch"], 1, stamp=temp / "gap.stamp")
        assert gap.returncode == WATCH_BROKEN_RC
        forbidden_launch.assert_not_called()

        class NeverReaps:
            pid = 99

            @staticmethod
            def wait(timeout):
                raise subprocess.TimeoutExpired("never", timeout)

        assert "did not reap" in (_bounded_reap(NeverReaps(), timeout=0.001) or "")

        fast_fifo = temp / "fast.fifo"
        os.mkfifo(fast_fifo)
        fast_kernel = [
            sys.executable, "-u", "-c",
            f"import time; print(open({str(fast_fifo)!r}).read(), end=''); time.sleep(30)",
        ]
        fast_child = [
            sys.executable, "-c", f"open({str(fast_fifo)!r},'w').write({planted_feed!r})",
        ]
        fast = run_guarded(
            fast_child, 5, incident_dir=temp / "fast", stamp=temp / "fast.stamp",
            kernel_command=fast_kernel, include_flight=False,
        )
        assert fast.returncode == WATCH_FAULT_RC, fast

        orphan_fifo = temp / "orphan.fifo"
        os.mkfifo(orphan_fifo)
        orphan_marker = temp / "orphan.pid"
        orphan_kernel = [
            sys.executable, "-u", "-c",
            f"import time; print(open({str(orphan_fifo)!r}).read(), end=''); time.sleep(30)",
        ]
        orphan_script = (
            "import subprocess,sys; "
            "p=subprocess.Popen([sys.executable,'-c','import time; time.sleep(30)']); "
            f"open({str(orphan_marker)!r},'w').write(str(p.pid)); "
            f"open({str(orphan_fifo)!r},'w').write('kernel: harmless control\\n')"
        )
        orphan = run_guarded(
            [sys.executable, "-c", orphan_script], 5,
            incident_dir=temp / "orphan", stamp=temp / "orphan.stamp",
            kernel_command=orphan_kernel, include_flight=False,
        )
        assert orphan.returncode == 0, orphan
        _wait_process_stopped(int(orphan_marker.read_text()))

        failed_fifo = temp / "failed-orphan.fifo"
        os.mkfifo(failed_fifo)
        failed_marker = temp / "failed-orphan.pid"
        failed_kernel = [
            sys.executable, "-u", "-c",
            f"print(open({str(failed_fifo)!r}).read(), end='')",
        ]
        failed_script = (
            "import subprocess,sys; "
            "p=subprocess.Popen([sys.executable,'-c','import time; time.sleep(30)']); "
            f"open({str(failed_marker)!r},'w').write(str(p.pid)); "
            f"open({str(failed_fifo)!r},'w').write('kernel: harmless then watcher EOF\\n')"
        )
        failed = run_guarded(
            [sys.executable, "-c", failed_script], 5,
            incident_dir=temp / "failed-orphan", stamp=temp / "failed-orphan.stamp",
            kernel_command=failed_kernel, include_flight=False,
        )
        assert failed.returncode == WATCH_BROKEN_RC, failed
        _wait_process_stopped(int(failed_marker.read_text()))

        broken_kernel = [sys.executable, "-c", "raise SystemExit(7)"]
        broken = run_guarded(
            [sys.executable, "-c", "import time; time.sleep(30)"], 5,
            incident_dir=temp / "broken", stamp=temp / "broken.stamp",
            kernel_command=broken_kernel, include_flight=False,
        )
        assert broken.returncode == WATCH_BROKEN_RC, broken
        assert (temp / "broken.stamp").is_file()

        early_child_marker = temp / "early-signal-child"
        early_signal_kernel = [
            sys.executable, "-c",
            "import os,signal,time; os.kill(os.getppid(), signal.SIGTERM); "
            "print('signal-sent', flush=True); time.sleep(30)",
        ]
        real_journal_start = JournalPump.start

        def synchronized_journal_start(pump: JournalPump) -> None:
            real_journal_start(pump)
            signal_line = pump.lines.get(timeout=1)
            assert signal_line == "signal-sent"
            pump.lines.put(signal_line)

        with mock.patch.object(JournalPump, "start", synchronized_journal_start):
            early_signal = run_guarded(
                [sys.executable, "-c",
                 f"open({str(early_child_marker)!r},'w').write('launched')"],
                5, incident_dir=temp / "early-signal", stamp=temp / "early-signal.stamp",
                kernel_command=early_signal_kernel, include_flight=False,
            )
        assert early_signal.returncode == 143
        assert not early_child_marker.exists(), "signal before child creation still launched child"

        signal_child_marker = temp / "signal-child.pids"
        signal_journal_marker = temp / "signal-journal.pid"
        guard_pid = os.fork()
        if guard_pid == 0:
            signal_kernel = [
                sys.executable, "-c",
                f"import os,time; open({str(signal_journal_marker)!r},'w').write(str(os.getpid())); "
                "time.sleep(30)",
            ]
            signal_script = (
                "import os,subprocess,sys,time; time.sleep(.1); "
                "p=subprocess.Popen([sys.executable,'-c','import time; time.sleep(30)']); "
                f"open({str(signal_child_marker)!r},'w').write(str(os.getpid())+' '+str(p.pid)); "
                "time.sleep(30)"
            )
            result = run_guarded(
                [sys.executable, "-c", signal_script], 5,
                incident_dir=temp / "signal", stamp=temp / "signal.stamp",
                kernel_command=signal_kernel, include_flight=False,
            )
            os._exit(result.returncode)
        _wait_for_path(signal_child_marker)
        _wait_for_path(signal_journal_marker)
        leader_pid, signal_descendant = map(int, signal_child_marker.read_text().split())
        signal_journal_pid = int(signal_journal_marker.read_text())
        os.kill(guard_pid, signal.SIGTERM)
        _waited, guard_status = os.waitpid(guard_pid, 0)
        assert os.waitstatus_to_exitcode(guard_status) == 143
        _wait_process_stopped(leader_pid)
        _wait_process_stopped(signal_descendant)
        _wait_process_stopped(signal_journal_pid)

    print("gpu-watch selftest PASS")
    print("  known-negative harmless journal line: command exits normally, no cooldown stamp")
    print("  known-positive illegal-register line: exact group killed before durable incident writes")
    print("  nanosecond timestamp reaches fake flight reader and emits CAUSAL-WINDOW output")
    print("  durable-write control observes file and parent-directory fsync")
    print("  interrupted atomic enrichment preserves the prior minimal incident")
    print("  stale-flight, reader-timeout, secret-redaction and preflight-gap controls pass")
    print("  bounded-reap control reports timeout without blocking")
    print("  fast-exit fault drains; clean leader exit kills its surviving descendant")
    print("  watcher-loss controls kill live leader and post-leader descendant")
    print("  early SIGTERM forbids child launch; live SIGTERM kills leader, descendant and follower")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--timeout", type=float, default=240.0, help="wall-clock cap in seconds")
    parser.add_argument("--output-log", type=Path, help="tee guarded process output to this file")
    parser.add_argument("--selftest", action="store_true")
    parser.add_argument("command", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    if args.selftest:
        return selftest()
    command = args.command
    if command and command[0] == "--":
        command = command[1:]
    if not command:
        parser.error("a command is required after --")
    if not math.isfinite(args.timeout) or not 0 < args.timeout <= MAX_GUARD_TIMEOUT_SECS:
        parser.error(f"--timeout must be finite and in (0,{MAX_GUARD_TIMEOUT_SECS:g}]")
    result = run_guarded(command, args.timeout, output_log=args.output_log)
    if result.returncode == 0:
        print("[gpu-watch] no kernel GPU fault observed through the final post-exit cursor scan; "
              "the next preflight still checks for delayed kernel evidence.")
    return result.returncode


if __name__ == "__main__":
    raise SystemExit(main())

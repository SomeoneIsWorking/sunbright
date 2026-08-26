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
import threading
import time
from dataclasses import dataclass
from datetime import datetime, timedelta, timezone
from pathlib import Path

from gpu_events import (
    DEFAULT_DEVCOREDUMP_ROOT,
    DeviceCoredumpEvidence,
    atomic_durable_replace,
    capture_new_device_coredumps,
    current_boot_id,
    current_kernel_cursor,
    device_coredump_snapshot,
    is_gpu_fault,
    kernel_lines_after_cursor,
)
from gpu_preflight import COOLDOWN_SECS, preflight_reasons
from radv_hang_trace import (
    DEFAULT_RADV_DUMP_ROOT,
    RadvDumpSnapshot,
    RadvHangEvidence,
    collect_radv_hang_trace,
    configure_radv_hang_environment,
    radv_hang_enabled,
    snapshot_radv_dumps,
)

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
PCI_DEVICE = re.compile(
    r"\b(?:[0-9a-f]{4}:)?[0-9a-f]{2}:[0-9a-f]{2}\.[0-7]\b", re.IGNORECASE
)


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
            raise RuntimeError(
                "GPU guard must run on the main thread for signal-safe cleanup"
            )
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
                command,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                bufsize=1,
            )
            self.journal = process
            return process
        finally:
            signal.pthread_sigmask(signal.SIG_SETMASK, current_mask)

    def popen_child(self, command: list[str]) -> subprocess.Popen[str]:
        current_mask = signal.pthread_sigmask(signal.SIG_BLOCK, self.handled)
        try:
            process = subprocess.Popen(
                command,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
                errors="replace",
                bufsize=1,
                start_new_session=True,
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


def _bounded_reap(
    process: subprocess.Popen[str], timeout: float = REAP_TIMEOUT_SECS
) -> str | None:
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
    local_second = datetime.strptime(
        match.group("second"), "%Y-%m-%dT%H:%M:%S"
    ).replace(tzinfo=zone)
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
            path
            for path in incident_dir.glob(f"session_{child_pid}_*.flight")
            if flight_snapshot.get(str(path.resolve()))
            != (path.stat().st_ino, path.stat().st_size, path.stat().st_mtime_ns)
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
        return (
            "reader output rejected: flight identity does not match guarded launch "
            f"({expected_pid}, {expected_session})\n{text}"
        )
    return text or f"gpu_flight_dump exited {result.returncode} without output"


def _redacted_command(command: list[str]) -> list[str]:
    redacted: list[str] = []
    for argument in command:
        name, separator, _value = argument.partition("=")
        redacted.append(
            f"{name}=<redacted>" if separator and SECRET_NAME.search(name) else argument
        )
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
            revision = (
                revision_path.read_text().strip()
                if revision_path.exists()
                else "unknown"
            )
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
    incident = (
        incident_dir / f"watch_{timestamp.strftime('%Y%m%dT%H%M%S.%fZ')}_{pgid}.txt"
    )
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
    devcoredump_evidence: list[DeviceCoredumpEvidence] | None = None,
    devcoredump_snapshot_error: str | None = None,
    radv_hang_evidence: RadvHangEvidence | None = None,
) -> Path:
    timestamp = datetime.now(timezone.utc)
    sections = [
        "Sunbright live GPU-fault guard incident",
        f"utc: {timestamp.isoformat()}",
        f"guarded pid/pgid: {child_pid}",
        f"command: {_redacted_command(command)!r}",
        f"STOP CATEGORY: {category}",
        f"FIRST STOP EVIDENCE: {reason}",
        (
            f"first kernel real_ns: {kernel_real_ns}"
            if kernel_real_ns is not None
            else "first kernel real_ns: UNAVAILABLE (timestamp did not parse)"
        ),
        "",
        "--- guarded process output tail ---",
        *(output_tail or ["<no process output captured>"]),
    ]
    if reap_note is not None:
        sections.extend(("", f"REAP STATUS: {reap_note}"))
    if live_kernel_lines:
        sections.extend(
            (
                "",
                "--- live kernel lines from the causal anchor ---",
                *live_kernel_lines,
            )
        )
    if include_static:
        sections.extend(
            ("", "--- static CPU-only environment ---", *_static_environment(command))
        )
    if include_context:
        sections.extend(("", "--- current kernel tail ---", _kernel_context()))
    if category == "gpu_fault":
        sections.extend(("", "--- Linux device-coredump evidence ---"))
        if devcoredump_snapshot_error is not None:
            sections.append(
                f"pre-launch snapshot: UNAVAILABLE ({devcoredump_snapshot_error})"
            )
        if devcoredump_evidence:
            for index, evidence in enumerate(devcoredump_evidence, start=1):
                sections.extend((f"candidate {index}:", *evidence.report))
        else:
            sections.append("status: UNAVAILABLE (capture produced no status record)")
    if radv_hang_evidence is not None:
        sections.extend(
            (
                "",
                "--- RADV hang diagnostic evidence ---",
                f"status: {radv_hang_evidence.status}",
                *radv_hang_evidence.report,
            )
        )
    if include_flight and category == "gpu_fault":
        sections.extend(
            (
                "",
                "--- matching submit-flight analysis ---",
                _flight_analysis(
                    child_pid,
                    kernel_real_ns,
                    flight_snapshot,
                    incident_dir,
                    flight_reader,
                ),
            )
        )
    atomic_durable_replace(incident, "\n".join(sections) + "\n")
    return incident


def _collect_fault_context(journal_pump: JournalPump, first_line: str) -> list[str]:
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
    devcoredump_root: Path = DEFAULT_DEVCOREDUMP_ROOT,
    radv_dump_root: Path = DEFAULT_RADV_DUMP_ROOT,
) -> GuardResult:
    try:
        configure_radv_hang_environment(os.environ)
    except ValueError as exc:
        reason = f"unsafe RADV hang diagnostic environment: {exc}"
        print(f"[gpu-watch] REFUSING: {reason}", file=sys.stderr)
        return GuardResult(WATCH_BROKEN_RC, reason)
    with SignalProtection() as signal_protection:
        return _run_guarded_protected(
            command,
            timeout_secs,
            output_log,
            incident_dir,
            stamp,
            kernel_command,
            include_flight,
            devcoredump_root,
            radv_dump_root,
            signal_protection,
        )


def _run_guarded_protected(
    command: list[str],
    timeout_secs: float,
    output_log: Path | None,
    incident_dir: Path,
    stamp: Path,
    kernel_command: list[str] | None,
    include_flight: bool,
    devcoredump_root: Path,
    radv_dump_root: Path,
    signal_protection: SignalProtection,
) -> GuardResult:
    start_cursor: str | None = None
    if kernel_command is None:
        start_cursor, error = current_kernel_cursor()
        if error is not None or start_cursor is None:
            print(
                f"[gpu-watch] REFUSING: cannot establish kernel cursor: {error}",
                file=sys.stderr,
            )
            return GuardResult(WATCH_BROKEN_RC)
        reasons = preflight_reasons(COOLDOWN_SECS, stamp)
        gap_lines, gap_error = kernel_lines_after_cursor(start_cursor)
        if gap_error is not None:
            reasons.append(f"could not inspect preflight/cursor gap: {gap_error}")
        reasons.extend(
            f"GPU fault appeared during preflight: {line}"
            for line in gap_lines
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
        print(
            f"[gpu-watch] REFUSING: cannot start kernel watcher: {exc}", file=sys.stderr
        )
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
        if barrier_error is not None or any(
            is_gpu_fault(line) for line in barrier_lines
        ):
            _stop_journal(journal)
            journal_pump.join(timeout=REAP_TIMEOUT_SECS)
            error_pump.join(timeout=REAP_TIMEOUT_SECS)
            reason = barrier_error or next(
                line for line in barrier_lines if is_gpu_fault(line)
            )
            print(
                f"[gpu-watch] REFUSING AT PRE-LAUNCH BARRIER: {reason}", file=sys.stderr
            )
            return GuardResult(WATCH_BROKEN_RC, reason)

    pgid: int | None = None
    flight_snapshot = {
        str(path.resolve()): (
            path.stat().st_ino,
            path.stat().st_size,
            path.stat().st_mtime_ns,
        )
        for path in incident_dir.glob("session_*.flight")
    }
    devcoredump_before, devcoredump_snapshot_error = device_coredump_snapshot(
        devcoredump_root
    )
    radv_snapshot: RadvDumpSnapshot | None = None
    if radv_hang_enabled(os.environ):
        radv_snapshot = snapshot_radv_dumps(radv_dump_root)
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

        def fault_result(
            reason: str, code: int, category: str = "gpu_fault"
        ) -> GuardResult:
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
                if category == "gpu_fault"
                else [reason]
            )
            devcoredump_evidence: list[DeviceCoredumpEvidence] | None = None
            if category == "gpu_fault":
                destination_prefix = incident or (
                    incident_dir / f"watch_unpersisted_{pgid}"
                )
                try:
                    devcoredump_evidence = capture_new_device_coredumps(
                        devcoredump_before,
                        destination_prefix,
                        root=devcoredump_root,
                        creation_announced=any(
                            "device coredump file has been created" in line.lower()
                            for line in live_kernel_lines
                        ),
                        expected_device_ids={
                            match.group(0)
                            for line in live_kernel_lines
                            for match in PCI_DEVICE.finditer(line)
                        },
                    )
                except Exception as exc:
                    devcoredump_evidence = [
                        DeviceCoredumpEvidence(
                            "capture-failed",
                            ("status: CAPTURE-FAILED", f"detail: {exc}"),
                        )
                    ]
            radv_hang_evidence: RadvHangEvidence | None = None
            if radv_snapshot is not None:
                destination_prefix = incident or (
                    incident_dir / f"watch_unpersisted_{pgid}"
                )
                try:
                    radv_hang_evidence = collect_radv_hang_trace(
                        radv_snapshot, child.pid, destination_prefix
                    )
                except Exception as exc:
                    radv_hang_evidence = RadvHangEvidence(
                        "UNAVAILABLE",
                        (f"RADV hang evidence collection failed safely: {exc}",),
                    )
            if persist_error is not None:
                print(f"[gpu-watch] {persist_error}", file=sys.stderr)
            if incident is not None:
                try:
                    _write_incident(
                        incident_dir,
                        incident,
                        child.pid,
                        command,
                        reason,
                        pump.snapshot(),
                        include_flight=include_flight,
                        flight_snapshot=flight_snapshot,
                        live_kernel_lines=live_kernel_lines,
                        kernel_real_ns=kernel_ns,
                        reap_note=reap_note,
                        category=category,
                        devcoredump_evidence=devcoredump_evidence,
                        devcoredump_snapshot_error=devcoredump_snapshot_error,
                        radv_hang_evidence=radv_hang_evidence,
                    )
                except Exception as exc:
                    print(
                        f"[gpu-watch] enrichment failed; minimal incident survives: {exc}",
                        file=sys.stderr,
                    )
            label = str(incident) if incident is not None else "UNAVAILABLE"
            print(
                f"[gpu-watch] stopped exact pid/pgid {pgid}; incident: {label}",
                file=sys.stderr,
            )
            return GuardResult(code, reason, incident)

        while True:
            if signal_protection.received:
                _kill_exact_group(pgid)
                note = _bounded_reap(child)
                if note:
                    print(f"[gpu-watch] {note}", file=sys.stderr)
                return GuardResult(128 + signal_protection.received)
            if time.monotonic() >= deadline:
                print(
                    f"[gpu-watch] wall-clock cap {timeout_secs:g}s reached; killing pid/pgid "
                    f"{pgid}.",
                    file=sys.stderr,
                )
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

            if (
                journal.poll() is not None
                and journal_pump.eof.is_set()
                and journal_pump.lines.empty()
            ):
                details = "; ".join(error_pump.lines) or "no diagnostic output"
                reason = f"kernel watcher exited {journal.returncode}: {details}"
                return fault_result(reason, WATCH_BROKEN_RC, "monitor_failure")

            leader_rc = child.poll()
            if leader_rc is not None:
                if leader_exit_deadline is None:
                    leader_exit_deadline = time.monotonic() + POST_EXIT_SETTLE_SECS
                if (
                    time.monotonic() >= leader_exit_deadline
                    and journal_pump.lines.empty()
                ):
                    if start_cursor is not None:
                        final_lines, final_error = kernel_lines_after_cursor(
                            start_cursor
                        )
                        if final_error is not None:
                            return fault_result(
                                f"post-exit kernel barrier failed: {final_error}",
                                WATCH_BROKEN_RC,
                                "monitor_failure",
                            )
                        first_fault = next(
                            (
                                candidate
                                for candidate in final_lines
                                if is_gpu_fault(candidate)
                            ),
                            None,
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


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--timeout", type=float, default=240.0, help="wall-clock cap in seconds"
    )
    parser.add_argument(
        "--output-log", type=Path, help="tee guarded process output to this file"
    )
    parser.add_argument("--selftest", action="store_true")
    parser.add_argument("command", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    if args.selftest:
        from gpu_watch_selftest import selftest

        return selftest()
    command = args.command
    if command and command[0] == "--":
        command = command[1:]
    if not command:
        parser.error("a command is required after --")
    if (
        not math.isfinite(args.timeout)
        or not 0 < args.timeout <= MAX_GUARD_TIMEOUT_SECS
    ):
        parser.error(f"--timeout must be finite and in (0,{MAX_GUARD_TIMEOUT_SECS:g}]")
    result = run_guarded(command, args.timeout, output_log=args.output_log)
    if result.returncode == 0:
        print(
            "[gpu-watch] no kernel GPU fault observed through the final post-exit cursor scan; "
            "the next preflight still checks for delayed kernel evidence."
        )
    return result.returncode


if __name__ == "__main__":
    raise SystemExit(main())

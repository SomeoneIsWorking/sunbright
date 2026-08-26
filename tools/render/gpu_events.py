#!/usr/bin/env python3
"""Shared kernel-event definitions for Sunbright's GPU safety interlocks."""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
import re
import signal
import stat
import subprocess
import sys
import tempfile
import time
from dataclasses import dataclass
from pathlib import Path
from unittest import mock

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


@dataclass(frozen=True)
class DeviceCoredumpEvidence:
    """Bounded result of preserving one newly-created Linux device coredump."""

    status: str
    report: tuple[str, ...]
    artifact: Path | None = None


DEFAULT_DEVCOREDUMP_ROOT = Path("/sys/class/devcoredump")
DEVCOREDUMP_MAX_BYTES = 64 * 1024 * 1024
DEVCOREDUMP_MAX_NODES = 2
DEVCOREDUMP_READ_SECS = 3.0
DEVCOREDUMP_WAIT_SECS = 1.0
DeviceCoredumpIdentity = tuple[int, int, int]


def atomic_durable_replace(path: Path, text: str) -> None:
    """Atomically replace a small safety record and fsync its directory entry."""
    path.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary_name = tempfile.mkstemp(
        prefix=f".{path.name}.", dir=path.parent
    )
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


def _device_coredump_nodes(root: Path) -> tuple[list[Path], str | None]:
    try:
        nodes = sorted(
            child
            for child in root.iterdir()
            if child.name != "disabled" and child.is_dir()
        )
    except FileNotFoundError:
        return [], f"device-coredump class unavailable: {root} does not exist"
    except PermissionError as exc:
        return [], f"device-coredump class permission denied: {exc}"
    except OSError as exc:
        return [], f"device-coredump class unavailable: {exc}"
    return nodes, None


def device_coredump_snapshot(
    root: Path = DEFAULT_DEVCOREDUMP_ROOT,
) -> tuple[dict[str, DeviceCoredumpIdentity], str | None]:
    """Snapshot existing nodes so a later fault never claims a stale dump as causal."""
    nodes, error = _device_coredump_nodes(root)
    if error is not None:
        return {}, error
    snapshot: dict[str, DeviceCoredumpIdentity] = {}
    snapshot_error: str | None = None
    for node in nodes:
        try:
            metadata = node.stat()
        except OSError as exc:
            if snapshot_error is None:
                snapshot_error = f"could not snapshot existing node {node}: {exc}"
            continue
        snapshot[str(node)] = (
            metadata.st_ino,
            metadata.st_ctime_ns,
            metadata.st_mtime_ns,
        )
    return snapshot, snapshot_error


def _device_coredump_disabled(root: Path) -> tuple[bool, str | None]:
    path = root / "disabled"
    try:
        value = path.read_text(encoding="ascii", errors="replace").strip()
    except FileNotFoundError:
        return False, f"device-coredump disable state unavailable: {path} is absent"
    except PermissionError as exc:
        return False, f"device-coredump disable state permission denied: {exc}"
    except OSError as exc:
        return False, f"device-coredump disable state unavailable: {exc}"
    if value == "1":
        return True, f"device-coredump capture disabled by {path}"
    if value not in ("", "0"):
        return False, f"device-coredump disable state is unrecognized: {value!r}"
    return False, None


def _device_coredump_failing_device(node: Path) -> tuple[str | None, str | None]:
    failing_device = node / "failing_device"
    try:
        return str(failing_device.resolve(strict=True)), None
    except FileNotFoundError:
        return None, "symlink target expired or absent"
    except PermissionError as exc:
        return None, f"permission denied: {exc}"
    except OSError as exc:
        return None, f"unavailable: {exc}"


def _device_coredump_metadata(node: Path) -> list[str]:
    lines = [f"source node: {node}"]
    data = node / "data"
    try:
        metadata = data.stat()
    except FileNotFoundError:
        lines.append("data metadata: EXPIRED (node has no data file)")
    except PermissionError as exc:
        lines.append(f"data metadata: PERMISSION-DENIED ({exc})")
    except OSError as exc:
        lines.append(f"data metadata: UNAVAILABLE ({exc})")
    else:
        lines.append(
            "data metadata: "
            f"mode={stat.filemode(metadata.st_mode)} uid={metadata.st_uid} gid={metadata.st_gid}"
        )
    failing_device, failing_error = _device_coredump_failing_device(node)
    if failing_device is not None:
        lines.append(f"failing device: {failing_device}")
    else:
        lines.append(f"failing device: UNAVAILABLE ({failing_error})")
    uevent = node / "uevent"
    try:
        uevent_text = uevent.read_text(encoding="utf-8", errors="replace")[
            :8192
        ].strip()
    except FileNotFoundError:
        lines.append("uevent: UNAVAILABLE (absent)")
    except PermissionError as exc:
        lines.append(f"uevent: PERMISSION-DENIED ({exc})")
    except OSError as exc:
        lines.append(f"uevent: UNAVAILABLE ({exc})")
    else:
        lines.append(f"uevent: {uevent_text if uevent_text else '<empty>'}")
    return lines


def _copy_device_coredump(
    source: Path,
    destination: Path,
    max_bytes: int,
    timeout_secs: float,
) -> tuple[str, int, str, str | None]:
    """Copy through a killable worker so blocking sysfs I/O cannot exceed the cap."""
    destination.parent.mkdir(parents=True, exist_ok=True)
    descriptor, staging_name = tempfile.mkstemp(
        prefix=f".{destination.name}.", dir=destination.parent
    )
    os.close(descriptor)
    staging = Path(staging_name)
    worker: subprocess.Popen[str] | None = None
    try:
        worker = subprocess.Popen(
            [
                sys.executable,
                str(Path(__file__).resolve()),
                "--copy-devcoredump-worker",
                str(source),
                str(staging),
                str(max_bytes),
            ],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            start_new_session=True,
        )
        try:
            stdout, stderr = worker.communicate(timeout=timeout_secs)
        except subprocess.TimeoutExpired:
            worker.kill()
            try:
                worker.communicate(timeout=1.0)
            except subprocess.TimeoutExpired:
                return (
                    "timeout-unreaped",
                    0,
                    hashlib.sha256().hexdigest(),
                    f"reader pid {worker.pid} remained unreaped after SIGKILL",
                )
            return (
                "timeout",
                0,
                hashlib.sha256().hexdigest(),
                f"reader pid {worker.pid} exceeded {timeout_secs:g}s; SIGKILLed and reaped",
            )
        if worker.returncode != 0:
            diagnostic = stderr.strip()[:400] or "worker produced no diagnostic output"
            return (
                "capture-worker-failed",
                0,
                hashlib.sha256().hexdigest(),
                f"reader pid {worker.pid} exited {worker.returncode}: {diagnostic}",
            )
        try:
            payload = json.loads(stdout)
            status = str(payload["status"])
            copied = int(payload["copied"])
            digest = str(payload["digest"])
            detail_value = payload.get("detail")
            artifact_ready_value = payload["artifact_ready"]
        except (json.JSONDecodeError, KeyError, TypeError, ValueError) as exc:
            return (
                "capture-worker-failed",
                0,
                hashlib.sha256().hexdigest(),
                f"reader pid {worker.pid} returned an invalid result: {exc}",
            )
        if (
            status
            not in {
                "captured",
                "empty",
                "truncated",
                "permission-denied",
                "expired",
                "unavailable",
                "partial-permission-denied",
                "partial-expired",
                "partial-read-error",
            }
            or not 0 <= copied <= max_bytes
            or re.fullmatch(r"[0-9a-f]{64}", digest) is None
            or not isinstance(detail_value, (str, type(None)))
            or not isinstance(artifact_ready_value, bool)
        ):
            return (
                "capture-worker-failed",
                0,
                hashlib.sha256().hexdigest(),
                f"reader pid {worker.pid} returned values outside the result contract",
            )
        detail = detail_value
        artifact_ready = artifact_ready_value
        if not artifact_ready:
            return status, copied, digest, detail
        try:
            staged_size = staging.stat().st_size
        except OSError as exc:
            return (
                "capture-worker-failed",
                0,
                hashlib.sha256().hexdigest(),
                f"reader pid {worker.pid} returned no usable staging file: {exc}",
            )
        if staged_size != copied:
            return (
                "capture-worker-failed",
                0,
                hashlib.sha256().hexdigest(),
                f"reader pid {worker.pid} staged {staged_size} bytes but reported {copied}",
            )
        os.replace(staging, destination)
        directory = os.open(
            destination.parent, os.O_RDONLY | getattr(os, "O_DIRECTORY", 0)
        )
        try:
            os.fsync(directory)
        finally:
            os.close(directory)
        return status, copied, digest, detail
    finally:
        if worker is not None and worker.poll() is None:
            worker.kill()
            try:
                worker.wait(timeout=1.0)
            except subprocess.TimeoutExpired:
                pass
        try:
            staging.unlink()
        except FileNotFoundError:
            pass


def _copy_device_coredump_worker(
    source: Path,
    staging: Path,
    max_bytes: int,
) -> dict[str, object]:
    """Read one dump. The parent owns the deadline, exact PID, staging path, and reap."""
    digest = hashlib.sha256()
    copied = 0
    status = "captured"
    detail: str | None = None
    try:
        source_file = source.open("rb", buffering=0)
    except PermissionError as exc:
        return {
            "status": "permission-denied",
            "copied": 0,
            "digest": digest.hexdigest(),
            "detail": str(exc),
            "artifact_ready": False,
        }
    except FileNotFoundError as exc:
        return {
            "status": "expired",
            "copied": 0,
            "digest": digest.hexdigest(),
            "detail": str(exc),
            "artifact_ready": False,
        }
    except OSError as exc:
        return {
            "status": "unavailable",
            "copied": 0,
            "digest": digest.hexdigest(),
            "detail": str(exc),
            "artifact_ready": False,
        }
    with source_file, staging.open("wb", buffering=0) as output:
        while copied < max_bytes:
            try:
                chunk = source_file.read(min(1024 * 1024, max_bytes - copied))
            except PermissionError as exc:
                status, detail = "partial-permission-denied", str(exc)
                break
            except FileNotFoundError as exc:
                status, detail = "partial-expired", str(exc)
                break
            except OSError as exc:
                status, detail = "partial-read-error", str(exc)
                break
            if not chunk:
                break
            output.write(chunk)
            digest.update(chunk)
            copied += len(chunk)
        if copied == 0 and status == "captured":
            status = "empty"
            detail = "device-coredump data returned EOF before any evidence byte"
        if copied == max_bytes and status == "captured":
            try:
                extra = source_file.read(1)
            except OSError as exc:
                status, detail = "partial-read-error", str(exc)
            else:
                if extra:
                    status = "truncated"
                    detail = f"dump exceeds the {max_bytes}-byte capture cap"
        output.flush()
        os.fsync(output.fileno())
    return {
        "status": status,
        "copied": copied,
        "digest": digest.hexdigest(),
        "detail": detail,
        "artifact_ready": True,
    }


def capture_new_device_coredumps(
    before: dict[str, DeviceCoredumpIdentity],
    destination_prefix: Path,
    *,
    root: Path = DEFAULT_DEVCOREDUMP_ROOT,
    creation_announced: bool = False,
    wait_secs: float = DEVCOREDUMP_WAIT_SECS,
    read_secs: float = DEVCOREDUMP_READ_SECS,
    max_bytes: int = DEVCOREDUMP_MAX_BYTES,
    max_nodes: int = DEVCOREDUMP_MAX_NODES,
    expected_device_ids: set[str] | None = None,
) -> list[DeviceCoredumpEvidence]:
    """Preserve only new post-launch dumps and name every unavailable evidence state."""
    if not math.isfinite(wait_secs) or wait_secs < 0:
        raise ValueError("device-coredump wait_secs must be finite and non-negative")
    if not math.isfinite(read_secs) or read_secs <= 0:
        raise ValueError("device-coredump read_secs must be finite and positive")
    if max_bytes <= 0 or max_nodes <= 0:
        raise ValueError("device-coredump byte and node caps must be positive")
    disabled, disable_note = _device_coredump_disabled(root)
    if disabled:
        assert disable_note is not None
        return [DeviceCoredumpEvidence("disabled", ("status: DISABLED", disable_note))]
    deadline = time.monotonic() + max(0.0, wait_secs)
    new_nodes: list[Path] = []
    discovery_error: str | None = None
    while True:
        nodes, discovery_error = _device_coredump_nodes(root)
        for node in nodes:
            try:
                metadata = node.stat()
            except OSError:
                continue
            identity = (metadata.st_ino, metadata.st_ctime_ns, metadata.st_mtime_ns)
            if before.get(str(node)) != identity:
                new_nodes.append(node)
        if new_nodes or discovery_error is not None or time.monotonic() >= deadline:
            break
        time.sleep(min(0.02, max(0.0, deadline - time.monotonic())))

    if discovery_error is not None:
        return [
            DeviceCoredumpEvidence(
                "unavailable", ("status: UNAVAILABLE", discovery_error)
            )
        ]
    if not new_nodes:
        stale_note = (
            f"; {len(before)} pre-launch node(s) were excluded as stale"
            if before
            else ""
        )
        status = "EXPIRED-OR-CONSUMED" if creation_announced else "UNAVAILABLE"
        reason = (
            "the kernel announced a dump, but no new node remained readable"
            if creation_announced
            else "no new device-coredump node appeared inside the bounded post-fault window"
        )
        report = [f"status: {status}", f"reason: {reason}{stale_note}"]
        if disable_note is not None:
            report.append(f"availability note: {disable_note}")
        return [
            DeviceCoredumpEvidence(
                status.lower(),
                tuple(report),
            )
        ]

    results: list[DeviceCoredumpEvidence] = []
    unique_nodes = list(dict.fromkeys(new_nodes))
    for node in unique_nodes[:max_nodes]:
        failing_device, failing_error = _device_coredump_failing_device(node)
        normalized_expected = {
            identity.lower() for identity in (expected_device_ids or set())
        }
        if normalized_expected and failing_device is not None:
            normalized_device = failing_device.lower()
            matched = next(
                (
                    identity
                    for identity in normalized_expected
                    if normalized_device.endswith(identity)
                ),
                None,
            )
            if matched is None:
                results.append(
                    DeviceCoredumpEvidence(
                        "unrelated",
                        (
                            "status: UNRELATED",
                            f"source node: {node}",
                            f"failing device: {failing_device}",
                            f"expected kernel device(s): {sorted(normalized_expected)!r}",
                            "artifact: NOT-CAPTURED (shared PCI device key does not match)",
                        ),
                    )
                )
                continue
        artifact = destination_prefix.with_name(
            f"{destination_prefix.name}.devcoredump-{node.name}.bin"
        )
        metadata_lines = _device_coredump_metadata(node)
        status, copied, digest, detail = _copy_device_coredump(
            node / "data", artifact, max_bytes, read_secs
        )
        lines = [f"status: {status.upper()}", *metadata_lines]
        if normalized_expected:
            if failing_device is None:
                lines.append(
                    "correlation: UNAVAILABLE "
                    f"(expected {sorted(normalized_expected)!r}; {failing_error})"
                )
            else:
                lines.append(f"correlation: MATCHED kernel PCI device {failing_device}")
        else:
            lines.append(
                "correlation: UNAVAILABLE (kernel fault context named no PCI device)"
            )
        if disable_note is not None:
            lines.append(f"availability note: {disable_note}")
        if artifact.exists():
            lines.extend(
                (
                    f"artifact: {artifact}",
                    f"captured bytes: {copied}",
                    f"captured sha256: {digest}",
                )
            )
        else:
            artifact = None
            lines.append("artifact: UNAVAILABLE")
        if detail is not None:
            lines.append(f"detail: {detail}")
        results.append(DeviceCoredumpEvidence(status, tuple(lines), artifact))
    if len(unique_nodes) > max_nodes:
        results.append(
            DeviceCoredumpEvidence(
                "truncated-node-list",
                (
                    "status: TRUNCATED-NODE-LIST",
                    f"reason: {len(unique_nodes)} new nodes exceeded the {max_nodes}-node cap",
                ),
            )
        )
    return results


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
        return (
            [],
            f"journalctl exited {result.returncode}: {result.stderr.strip()[:200]}",
        )

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
        return (
            None,
            f"journalctl exited {result.returncode}: {result.stderr.strip()[:200]}",
        )
    for line in reversed(result.stdout.splitlines()):
        if line.startswith("-- cursor: "):
            return line.removeprefix("-- cursor: ").strip(), None
    return None, "journalctl did not return a kernel cursor"


def kernel_lines_after_cursor(cursor: str) -> tuple[list[str], str | None]:
    """Return all current-boot kernel lines appended after an exact journal cursor."""
    try:
        result = subprocess.run(
            [
                "journalctl",
                "-k",
                "-b",
                "--after-cursor",
                cursor,
                "--no-pager",
                "-o",
                "short-iso-precise",
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
        return (
            [],
            f"journalctl exited {result.returncode}: {result.stderr.strip()[:200]}",
        )
    return result.stdout.splitlines(), None


def current_boot_id() -> tuple[str | None, str | None]:
    try:
        with open("/proc/sys/kernel/random/boot_id", encoding="ascii") as source:
            boot_id = source.read().strip()
    except OSError as exc:
        return None, f"cannot read boot id: {exc}"
    return (boot_id, None) if boot_id else (None, "kernel returned an empty boot id")


def selftest() -> int:
    repo = Path(__file__).resolve().parents[2]
    scratch = repo / "scratch"
    scratch.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(
        prefix="gpu-events-selftest-", dir=scratch
    ) as temp_text:
        temp = Path(temp_text)
        no_dump_root = temp / "no-device-coredump"
        no_dump_root.mkdir()
        (no_dump_root / "disabled").write_text("0\n", encoding="ascii")

        readable_root = temp / "readable-device-coredump"
        readable_root.mkdir()
        (readable_root / "disabled").write_text("0\n", encoding="ascii")
        readable_before, readable_snapshot_error = device_coredump_snapshot(
            readable_root
        )
        assert readable_snapshot_error is None and not readable_before
        readable_node = readable_root / "devcd0"
        readable_node.mkdir()
        planted_dump = b"**** AMDGPU Device Coredump ****\nplanted-ring-packet\n"
        (readable_node / "data").write_bytes(planted_dump)
        (readable_node / "uevent").write_text("DRIVER=amdgpu\n", encoding="ascii")
        fake_device = temp / "0000:0b:00.0"
        fake_device.mkdir()
        (readable_node / "failing_device").symlink_to(fake_device)
        readable = capture_new_device_coredumps(
            readable_before,
            temp / "readable-incident",
            root=readable_root,
            wait_secs=0,
            read_secs=1,
            expected_device_ids={"0000:0b:00.0"},
        )
        assert [evidence.status for evidence in readable] == ["captured"]
        assert readable[0].artifact is not None
        assert readable[0].artifact.read_bytes() == planted_dump
        readable_report = "\n".join(readable[0].report)
        assert "DRIVER=amdgpu" in readable_report
        assert "0000:0b:00.0" in readable_report
        assert "captured sha256:" in readable_report
        assert "correlation: MATCHED" in readable_report
        readable_stale, _ = device_coredump_snapshot(readable_root)
        stale = capture_new_device_coredumps(
            readable_stale,
            temp / "stale-incident",
            root=readable_root,
            wait_secs=0,
        )
        assert stale[0].status == "unavailable"
        assert "excluded as stale" in "\n".join(stale[0].report)
        assert not list(temp.glob("stale-incident.devcoredump-*.bin"))

        unrelated_root = temp / "unrelated-device-coredump"
        unrelated_root.mkdir()
        (unrelated_root / "disabled").write_text("0\n", encoding="ascii")
        unrelated_node = unrelated_root / "devcd-other"
        unrelated_node.mkdir()
        (unrelated_node / "data").write_bytes(b"other device")
        other_device = temp / "0000:0c:00.0"
        other_device.mkdir()
        (unrelated_node / "failing_device").symlink_to(other_device)
        unrelated = capture_new_device_coredumps(
            {},
            temp / "unrelated-incident",
            root=unrelated_root,
            wait_secs=0,
            expected_device_ids={"0000:0b:00.0"},
        )
        assert unrelated[0].status == "unrelated"
        assert unrelated[0].artifact is None
        assert "shared PCI device key does not match" in "\n".join(unrelated[0].report)

        truncated_root = temp / "truncated-device-coredump"
        truncated_root.mkdir()
        (truncated_root / "disabled").write_text("0\n", encoding="ascii")
        truncated_node = truncated_root / "devcd1"
        truncated_node.mkdir()
        (truncated_node / "data").write_bytes(b"0123456789abcdef")
        truncated = capture_new_device_coredumps(
            {},
            temp / "truncated-incident",
            root=truncated_root,
            wait_secs=0,
            read_secs=1,
            max_bytes=8,
        )
        assert truncated[0].status == "truncated"
        assert truncated[0].artifact is not None
        assert truncated[0].artifact.read_bytes() == b"01234567"

        empty_root = temp / "empty-device-coredump"
        empty_root.mkdir()
        (empty_root / "disabled").write_text("0\n", encoding="ascii")
        empty_node = empty_root / "devcd-empty"
        empty_node.mkdir()
        (empty_node / "data").write_bytes(b"")
        empty = capture_new_device_coredumps(
            {},
            temp / "empty-incident",
            root=empty_root,
            wait_secs=0,
            read_secs=1,
        )
        assert empty[0].status == "empty"
        assert "before any evidence byte" in "\n".join(empty[0].report)

        denied_root = temp / "denied-device-coredump"
        denied_root.mkdir()
        (denied_root / "disabled").write_text("0\n", encoding="ascii")
        denied_data = denied_root / "devcd2" / "data"
        denied_data.parent.mkdir()
        denied_data.write_bytes(b"must-not-be-captured")
        real_path_open = Path.open

        def deny_fixture(path: Path, *args, **kwargs):
            if path == denied_data:
                raise PermissionError("planted sysfs 0600 control")
            return real_path_open(path, *args, **kwargs)

        with mock.patch.object(Path, "open", deny_fixture):
            denied_payload = _copy_device_coredump_worker(
                denied_data, temp / "denied-staging", 1024
            )
        assert denied_payload["status"] == "permission-denied"
        assert denied_payload["artifact_ready"] is False
        assert "planted sysfs 0600 control" in str(denied_payload["detail"])
        assert not list(temp.glob("denied-incident.devcoredump-*.bin"))

        timeout_root = temp / "timeout-device-coredump"
        timeout_root.mkdir()
        (timeout_root / "disabled").write_text("0\n", encoding="ascii")
        timeout_node = timeout_root / "devcd-blocked"
        timeout_node.mkdir()
        os.mkfifo(timeout_node / "data")
        launched_readers: list[subprocess.Popen[str]] = []
        real_popen = subprocess.Popen

        def observe_reader(*args, **kwargs):
            reader = real_popen(*args, **kwargs)
            launched_readers.append(reader)
            return reader

        started = time.monotonic()
        with mock.patch.object(subprocess, "Popen", side_effect=observe_reader):
            timed_out = capture_new_device_coredumps(
                {},
                temp / "timeout-incident",
                root=timeout_root,
                wait_secs=0,
                read_secs=0.15,
            )
        elapsed = time.monotonic() - started
        assert timed_out[0].status == "timeout"
        assert timed_out[0].artifact is None
        assert "SIGKILLed and reaped" in "\n".join(timed_out[0].report)
        assert elapsed < 1.0, f"blocked reader escaped its deadline: {elapsed:.3f}s"
        assert len(launched_readers) == 1
        assert launched_readers[0].poll() is not None
        assert launched_readers[0].returncode == -signal.SIGKILL
        assert not list(temp.glob(".timeout-incident.devcoredump-devcd-blocked.bin.*"))

        expired_root = temp / "expired-device-coredump"
        expired_root.mkdir()
        (expired_root / "disabled").write_text("0\n", encoding="ascii")
        (expired_root / "devcd3").mkdir()
        expired = capture_new_device_coredumps(
            {},
            temp / "expired-incident",
            root=expired_root,
            wait_secs=0,
            read_secs=1,
        )
        assert expired[0].status == "expired"
        assert "EXPIRED" in "\n".join(expired[0].report)

        absent = capture_new_device_coredumps(
            {},
            temp / "absent-incident",
            root=temp / "absent-class",
            wait_secs=0,
        )
        assert absent[0].status == "unavailable"
        announced = capture_new_device_coredumps(
            {},
            temp / "announced-incident",
            root=no_dump_root,
            creation_announced=True,
            wait_secs=0,
        )
        assert announced[0].status == "expired-or-consumed"
        disabled_root = temp / "disabled-device-coredump"
        disabled_root.mkdir()
        (disabled_root / "disabled").write_text("1\n", encoding="ascii")
        disabled = capture_new_device_coredumps(
            {},
            temp / "disabled-incident",
            root=disabled_root,
            wait_secs=0,
        )
        assert disabled[0].status == "disabled"

    print("gpu-events selftest PASS")
    print("  readable dump is copied byte-exactly with identity metadata and hash")
    print(
        "  empty, truncated, denied, expired, announced-consumed and disabled disagree"
    )
    print(
        "  stale and PCI-mismatched nodes plus an absent class are never called captured"
    )
    print(
        "  blocked-open worker hits deadline, is SIGKILLed/reaped, and leaves no artifact"
    )
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--selftest", action="store_true")
    parser.add_argument(
        "--copy-devcoredump-worker",
        nargs=3,
        metavar=("SOURCE", "STAGING", "MAX_BYTES"),
        help=argparse.SUPPRESS,
    )
    args = parser.parse_args()
    if args.selftest and args.copy_devcoredump_worker is not None:
        parser.error("--selftest and the internal copy worker are mutually exclusive")
    if args.selftest:
        return selftest()
    if args.copy_devcoredump_worker is not None:
        source_text, staging_text, max_bytes_text = args.copy_devcoredump_worker
        try:
            max_bytes = int(max_bytes_text)
        except ValueError:
            parser.error("internal MAX_BYTES must be an integer")
        if max_bytes <= 0:
            parser.error("internal MAX_BYTES must be positive")
        result = _copy_device_coredump_worker(
            Path(source_text), Path(staging_text), max_bytes
        )
        print(json.dumps(result, sort_keys=True))
        return 0
    parser.error("no action requested")


if __name__ == "__main__":
    raise SystemExit(main())

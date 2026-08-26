#!/usr/bin/env python3
"""Preserve and summarize a RADV ``RADV_DEBUG=hang`` evidence directory.

This module is deliberately not a launcher.  ``RADV_DEBUG=hang`` inserts synchronization and
can mask a timing or lifetime defect, so callers must expose it only as an opt-in diagnostic lane.
The integration contract is:

1. call :func:`snapshot_radv_dumps` immediately before launching the guarded child;
2. retain the exact child PID returned by ``Popen``;
3. after that child exits, call :func:`collect_radv_hang_trace` with both values.

Only newly-created ``radv_dumps_<exact-child-pid>_<time>`` directories are eligible.  Evidence is
read without modifying its source and published by one atomic directory rename under the caller's
incident prefix.  Every scan and copy is bounded; silence is reported as UNKNOWN, not success.
"""

from __future__ import annotations

import argparse
import hashlib
import math
import os
import re
import stat
import tempfile
import time
from dataclasses import dataclass
from pathlib import Path
from unittest import mock

RADV_DUMP_NAME = re.compile(r"^radv_dumps_(?P<pid>[1-9][0-9]*)_(?P<stamp>.+)$")
TRACE_ID = re.compile(r"\bTrace ID:\s*(?:0x)?(?P<id>[0-9a-f]+)\b", re.IGNORECASE)
TRACE_LAST_REACHED = re.compile(
    r"last trace point that was reached by the CP", re.IGNORECASE
)
TRACE_NOT_REACHED = re.compile(r"trace point was NOT reached by the CP", re.IGNORECASE)
TRACE_REACHED = re.compile(r"trace point was reached by the CP", re.IGNORECASE)

DEFAULT_RADV_DUMP_ROOT = Path.home()
DEFAULT_WAIT_SECS = 1.0
DEFAULT_READ_SECS = 3.0
DEFAULT_MAX_FILES = 128
DEFAULT_MAX_BYTES = 64 * 1024 * 1024
COPY_CHUNK_BYTES = 1024 * 1024

DirectoryIdentity = tuple[int, int, int]


@dataclass(frozen=True)
class RadvDumpSnapshot:
    """Pre-launch identities used to exclude stale or replaced dump directories."""

    root: Path
    identities: dict[str, DirectoryIdentity]
    status: str
    detail: str


@dataclass(frozen=True)
class TraceProgress:
    """Command-processor progress decoded from one RADV ``trace.log``."""

    last_reached_id: int | None
    first_not_reached_id: int | None
    report: tuple[str, ...]
    complete: bool


@dataclass(frozen=True)
class RadvHangEvidence:
    """Bounded preservation result.  Status is never success-shaped on absent evidence."""

    status: str
    report: tuple[str, ...]
    artifact: Path | None = None


def configure_radv_hang_environment(environment: dict[str, str]) -> bool:
    """Apply the explicit launcher opt-in and return whether the lane is enabled."""
    requested = environment.get("SBR_RADV_HANG_DIAG", "0")
    if requested not in ("", "0", "1"):
        raise ValueError("SBR_RADV_HANG_DIAG must be 0 or 1")
    tokens = [token.strip() for token in environment.get("RADV_DEBUG", "").split(",")]
    selected = [token for token in tokens if token]
    if requested != "1":
        if "hang" in selected:
            raise ValueError(
                "RADV_DEBUG=hang requires the explicit SBR_RADV_HANG_DIAG=1 opt-in"
            )
        return False
    if "hang" not in selected:
        selected.append("hang")
    environment["RADV_DEBUG"] = ",".join(selected)
    return True


def radv_hang_enabled(environment: dict[str, str]) -> bool:
    """Return whether the effective RADV driver flags contain the exact hang token."""
    return "hang" in {
        token.strip() for token in environment.get("RADV_DEBUG", "").split(",")
    }


def _scan_dump_directories(
    root: Path,
) -> tuple[list[tuple[Path, int, DirectoryIdentity]], str | None, str | None]:
    try:
        children = list(root.iterdir())
    except PermissionError as exc:
        return [], "PERMISSION-DENIED", f"cannot scan RADV dump root {root}: {exc}"
    except (FileNotFoundError, NotADirectoryError) as exc:
        return [], "UNAVAILABLE", f"RADV dump root is unavailable ({root}): {exc}"
    except OSError as exc:
        return [], "UNAVAILABLE", f"cannot scan RADV dump root {root}: {exc}"

    dumps: list[tuple[Path, int, DirectoryIdentity]] = []
    for child in children:
        match = RADV_DUMP_NAME.fullmatch(child.name)
        if match is None:
            continue
        try:
            metadata = child.stat(follow_symlinks=False)
        except PermissionError as exc:
            return (
                [],
                "PERMISSION-DENIED",
                f"cannot inspect RADV dump directory {child}: {exc}",
            )
        except OSError:
            continue
        if not stat.S_ISDIR(metadata.st_mode) or stat.S_ISLNK(metadata.st_mode):
            continue
        dumps.append(
            (
                child,
                int(match.group("pid")),
                (metadata.st_ino, metadata.st_ctime_ns, metadata.st_mtime_ns),
            )
        )
    dumps.sort(key=lambda item: (item[2][1], item[0].name))
    return dumps, None, None


def snapshot_radv_dumps(root: Path = DEFAULT_RADV_DUMP_ROOT) -> RadvDumpSnapshot:
    """Capture pre-launch directory identities so stale evidence cannot be attributed later."""
    try:
        root = root.resolve()
    except PermissionError as exc:
        return RadvDumpSnapshot(
            root, {}, "PERMISSION-DENIED", f"cannot resolve {root}: {exc}"
        )
    except OSError as exc:
        return RadvDumpSnapshot(
            root, {}, "UNAVAILABLE", f"cannot resolve {root}: {exc}"
        )
    dumps, status, detail = _scan_dump_directories(root)
    if status is not None:
        return RadvDumpSnapshot(root, {}, status, detail or status)
    return RadvDumpSnapshot(
        root,
        {str(path): identity for path, _pid, identity in dumps},
        "READY",
        f"snapshotted {len(dumps)} existing RADV dump director{'y' if len(dumps) == 1 else 'ies'}",
    )


def parse_trace_log(text: str) -> TraceProgress:
    """Parse RADV's CP trace markers without inferring progress from unrecognized text."""
    current_id: int | None = None
    reached: list[tuple[int, bool]] = []
    not_reached: list[int] = []
    for line in text.splitlines():
        trace_match = TRACE_ID.search(line)
        if trace_match is not None:
            current_id = int(trace_match.group("id"), 16)
        if current_id is None:
            continue
        if TRACE_LAST_REACHED.search(line):
            reached.append((current_id, True))
        elif TRACE_NOT_REACHED.search(line):
            not_reached.append(current_id)
        elif TRACE_REACHED.search(line):
            reached.append((current_id, False))

    explicit_last = [trace_id for trace_id, is_last in reached if is_last]
    last_reached = (
        explicit_last[-1] if explicit_last else (reached[-1][0] if reached else None)
    )
    first_not_reached = not_reached[0] if not_reached else None
    report: list[str] = []
    if last_reached is None:
        report.append(
            "last CP-reached trace: UNKNOWN (no recognized RADV reached marker)"
        )
    else:
        qualifier = (
            "driver-marked last" if explicit_last else "last recognized reached marker"
        )
        report.append(f"last CP-reached trace: 0x{last_reached:x} ({qualifier})")
    if first_not_reached is None:
        report.append(
            "first CP-not-reached trace: UNKNOWN (no recognized RADV NOT-reached marker)"
        )
    else:
        report.append(f"first CP-not-reached trace: 0x{first_not_reached:x}")
    return TraceProgress(
        last_reached,
        first_not_reached,
        tuple(report),
        last_reached is not None and first_not_reached is not None,
    )


def _new_exact_pid_dumps(
    snapshot: RadvDumpSnapshot,
    child_pid: int,
) -> tuple[list[Path], int, int, str | None, str | None]:
    dumps, status, detail = _scan_dump_directories(snapshot.root)
    if status is not None:
        return [], 0, 0, status, detail
    exact: list[Path] = []
    stale = 0
    wrong_pid = 0
    for path, pid, identity in dumps:
        prior = snapshot.identities.get(str(path))
        if prior == identity:
            stale += 1
            continue
        if pid != child_pid:
            wrong_pid += 1
            continue
        exact.append(path)
    return exact, stale, wrong_pid, None, None


def _source_files(
    sources: list[Path], max_files: int, deadline: float
) -> tuple[list[tuple[Path, Path]], list[str], str | None]:
    files: list[tuple[Path, Path]] = []
    notes: list[str] = []
    permission_error: str | None = None
    for source in sources:
        stack: list[tuple[Path, Path]] = [(source, Path(source.name))]
        while stack:
            if time.monotonic() >= deadline:
                notes.append("PARTIAL: source discovery exceeded the read-time cap")
                return files, notes, permission_error
            directory, relative = stack.pop()
            try:
                children = sorted(directory.iterdir(), key=lambda path: path.name)
            except PermissionError as exc:
                permission_error = (
                    f"permission denied while scanning {directory}: {exc}"
                )
                notes.append(f"PERMISSION-DENIED: {permission_error}")
                continue
            except OSError as exc:
                notes.append(f"PARTIAL: could not scan {directory}: {exc}")
                continue
            for child in children:
                try:
                    metadata = child.stat(follow_symlinks=False)
                except PermissionError as exc:
                    permission_error = (
                        f"permission denied while inspecting {child}: {exc}"
                    )
                    notes.append(f"PERMISSION-DENIED: {permission_error}")
                    continue
                except OSError as exc:
                    notes.append(f"PARTIAL: could not inspect {child}: {exc}")
                    continue
                child_relative = relative / child.name
                if stat.S_ISDIR(metadata.st_mode) and not stat.S_ISLNK(
                    metadata.st_mode
                ):
                    stack.append((child, child_relative))
                elif stat.S_ISREG(metadata.st_mode) and not stat.S_ISLNK(
                    metadata.st_mode
                ):
                    if len(files) >= max_files:
                        notes.append(
                            f"PARTIAL: evidence exceeds the {max_files}-file cap"
                        )
                        return files, notes, permission_error
                    files.append((child, child_relative))
                else:
                    notes.append(f"PARTIAL: skipped non-regular evidence entry {child}")
    return files, notes, permission_error


def _discard_temporary_tree(root: Path) -> None:
    """Remove only our unpublished temporary destination; source evidence is never touched."""
    if not root.exists():
        return
    for path in sorted(root.rglob("*"), key=lambda item: len(item.parts), reverse=True):
        try:
            path.unlink() if not path.is_dir() else path.rmdir()
        except FileNotFoundError:
            pass
    try:
        root.rmdir()
    except FileNotFoundError:
        pass


def _copy_evidence(
    sources: list[Path],
    artifact: Path,
    *,
    max_files: int,
    max_bytes: int,
    read_secs: float,
) -> tuple[str, list[str]]:
    if artifact.exists():
        return "UNAVAILABLE", [
            f"refusing to overwrite existing RADV artifact {artifact}"
        ]
    try:
        artifact.parent.mkdir(parents=True, exist_ok=True)
        temporary = Path(
            tempfile.mkdtemp(prefix=f".{artifact.name}.", dir=artifact.parent)
        )
    except PermissionError as exc:
        return "PERMISSION-DENIED", [
            f"cannot create RADV incident destination under {artifact.parent}: {exc}"
        ]
    except OSError as exc:
        return "UNAVAILABLE", [
            f"cannot create RADV incident destination under {artifact.parent}: {exc}"
        ]
    deadline = time.monotonic() + max(0.0, read_secs)
    copied_files = 0
    copied_bytes = 0
    digest = hashlib.sha256()
    notes: list[str] = []
    denied = False
    try:
        files, discovery_notes, permission_error = _source_files(
            sources, max_files, deadline
        )
        notes.extend(discovery_notes)
        denied = permission_error is not None
        if not files and not notes:
            notes.append(
                "PARTIAL: eligible RADV directory contains no regular evidence files"
            )
        for source, relative in files:
            if time.monotonic() >= deadline:
                notes.append("PARTIAL: evidence copy exceeded the read-time cap")
                break
            remaining = max_bytes - copied_bytes
            if remaining <= 0:
                notes.append(f"PARTIAL: evidence exceeds the {max_bytes}-byte cap")
                break
            destination = temporary / relative
            file_bytes = 0
            try:
                destination.parent.mkdir(parents=True, exist_ok=True)
                with (
                    source.open("rb", buffering=0) as input_file,
                    destination.open("wb", buffering=0) as output_file,
                ):
                    while remaining > 0 and time.monotonic() < deadline:
                        chunk = input_file.read(min(COPY_CHUNK_BYTES, remaining))
                        if not chunk:
                            break
                        output_file.write(chunk)
                        digest.update(chunk)
                        file_bytes += len(chunk)
                        copied_bytes += len(chunk)
                        remaining -= len(chunk)
                    if time.monotonic() >= deadline:
                        notes.append(f"PARTIAL: timed out while copying {source}")
                    elif remaining == 0 and input_file.read(1):
                        notes.append(
                            f"PARTIAL: evidence exceeds the {max_bytes}-byte cap"
                        )
                    output_file.flush()
                    os.fsync(output_file.fileno())
            except PermissionError as exc:
                denied = True
                notes.append(f"PERMISSION-DENIED: could not read {source}: {exc}")
                try:
                    destination.unlink()
                except FileNotFoundError:
                    pass
                continue
            except OSError as exc:
                notes.append(f"PARTIAL: could not copy {source}: {exc}")
                try:
                    destination.unlink()
                except FileNotFoundError:
                    pass
                continue
            copied_files += 1
            notes.append(f"copied {relative}: {file_bytes} byte(s)")

        if copied_files == 0:
            return ("PERMISSION-DENIED" if denied else "PARTIAL"), notes
        try:
            os.replace(temporary, artifact)
        except PermissionError as exc:
            notes.append(f"PERMISSION-DENIED: atomic artifact publish failed: {exc}")
            return "PERMISSION-DENIED", notes
        except OSError as exc:
            notes.append(f"UNAVAILABLE: atomic artifact publish failed: {exc}")
            return "UNAVAILABLE", notes
        temporary = Path()
        try:
            directory = os.open(
                artifact.parent, os.O_RDONLY | getattr(os, "O_DIRECTORY", 0)
            )
            try:
                os.fsync(directory)
            finally:
                os.close(directory)
        except OSError as exc:
            notes.append(
                f"PARTIAL: artifact published but directory fsync failed: {exc}"
            )
        notes.append(f"artifact: {artifact}")
        notes.append(f"captured: {copied_files} file(s), {copied_bytes} byte(s)")
        notes.append(f"aggregate sha256: {digest.hexdigest()}")
        return (
            "PARTIAL"
            if any(note.startswith(("PARTIAL", "PERMISSION")) for note in notes)
            else "CAPTURED"
        ), notes
    finally:
        if temporary != Path():
            _discard_temporary_tree(temporary)


def collect_radv_hang_trace(
    snapshot: RadvDumpSnapshot,
    child_pid: int,
    incident_prefix: Path,
    *,
    wait_secs: float = DEFAULT_WAIT_SECS,
    read_secs: float = DEFAULT_READ_SECS,
    max_files: int = DEFAULT_MAX_FILES,
    max_bytes: int = DEFAULT_MAX_BYTES,
) -> RadvHangEvidence:
    """Preserve new exact-child RADV evidence after exit, with bounded discovery and reads."""
    if child_pid <= 0:
        return RadvHangEvidence(
            "UNAVAILABLE", (f"invalid exact child PID: {child_pid}",)
        )
    if snapshot.status != "READY":
        return RadvHangEvidence(
            snapshot.status,
            (f"pre-launch RADV snapshot was not usable: {snapshot.detail}",),
        )
    if (
        max_files <= 0
        or max_bytes <= 0
        or not math.isfinite(read_secs)
        or read_secs <= 0
        or not math.isfinite(wait_secs)
        or wait_secs < 0
    ):
        return RadvHangEvidence(
            "UNAVAILABLE",
            (
                "RADV evidence time bounds must be finite and byte/file/read bounds positive",
            ),
        )

    deadline = time.monotonic() + wait_secs
    exact: list[Path] = []
    stale = 0
    wrong_pid = 0
    while True:
        exact, stale, wrong_pid, scan_status, scan_detail = _new_exact_pid_dumps(
            snapshot, child_pid
        )
        if scan_status is not None:
            return RadvHangEvidence(scan_status, (scan_detail or scan_status,))
        if exact or time.monotonic() >= deadline:
            break
        time.sleep(min(0.05, max(0.0, deadline - time.monotonic())))

    attribution = (
        f"attribution: exact child pid={child_pid}; eligible={len(exact)}; "
        f"excluded stale={stale}; excluded wrong-pid={wrong_pid}"
    )
    if not exact:
        return RadvHangEvidence(
            "UNKNOWN",
            (
                attribution,
                (
                    "no new exact-child RADV dump appeared within the bounded post-exit window; "
                    "this does not prove that the GPU did not hang"
                ),
            ),
        )

    artifact = incident_prefix.with_name(f"{incident_prefix.name}.radv")
    status, copy_report = _copy_evidence(
        exact,
        artifact,
        max_files=max_files,
        max_bytes=max_bytes,
        read_secs=read_secs,
    )
    report = [
        attribution,
        "RADV_DEBUG=hang is diagnostic-only: its inserted synchronization can mask a timing defect",
        *copy_report,
    ]
    if not artifact.exists():
        return RadvHangEvidence(status, tuple(report))

    progress_complete = True
    trace_count = 0
    for source in exact:
        relative_trace = Path(source.name) / "trace.log"
        copied_trace = artifact / relative_trace
        if not copied_trace.exists():
            progress_complete = False
            report.append(
                f"{source.name}/trace.log: UNKNOWN (missing from captured evidence)"
            )
            continue
        trace_count += 1
        try:
            trace_text = copied_trace.read_text(encoding="utf-8", errors="replace")
        except PermissionError as exc:
            progress_complete = False
            report.append(f"{source.name}/trace.log: PERMISSION-DENIED ({exc})")
            continue
        except OSError as exc:
            progress_complete = False
            report.append(f"{source.name}/trace.log: UNAVAILABLE ({exc})")
            continue
        progress = parse_trace_log(trace_text)
        report.extend(f"{source.name}/{line}" for line in progress.report)
        progress_complete = progress_complete and progress.complete
    if trace_count == 0 or not progress_complete:
        status = "PARTIAL"
    return RadvHangEvidence(status, tuple(report), artifact)


def selftest() -> int:
    repo = Path(__file__).resolve().parents[2]
    scratch = repo / "scratch"
    scratch.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(
        prefix="radv-hang-selftest-", dir=scratch
    ) as temp_text:
        temp = Path(temp_text)

        positive_root = temp / "positive-root"
        positive_root.mkdir()
        positive_before = snapshot_radv_dumps(positive_root)
        positive_source = positive_root / "radv_dumps_4321_2026.08.27_01.02.03"
        positive_source.mkdir()
        trace_text = """Trace ID: a
This trace point was reached by the CP.
Trace ID: b
!!!!! This is the last trace point that was reached by the CP !!!!!
Trace ID: c
!!!!! This trace point was NOT reached by the CP !!!!!
"""
        (positive_source / "trace.log").write_text(trace_text, encoding="utf-8")
        (positive_source / "pipeline.log").write_text(
            "pipeline evidence\n", encoding="utf-8"
        )
        positive = collect_radv_hang_trace(
            positive_before, 4321, temp / "positive-incident", wait_secs=0
        )
        assert positive.status == "CAPTURED"
        assert positive.artifact is not None
        assert (
            positive.artifact / positive_source.name / "trace.log"
        ).read_text() == trace_text
        assert "last CP-reached trace: 0xb" in "\n".join(positive.report)
        assert "first CP-not-reached trace: 0xc" in "\n".join(positive.report)
        assert positive_source.exists(), "collector moved or deleted source evidence"

        empty_root = temp / "empty-root"
        empty_root.mkdir()
        empty_before = snapshot_radv_dumps(empty_root)
        empty = collect_radv_hang_trace(
            empty_before, 5000, temp / "empty-incident", wait_secs=0
        )
        assert empty.status == "UNKNOWN"
        assert "does not prove" in "\n".join(empty.report)

        excluded_root = temp / "excluded-root"
        excluded_root.mkdir()
        stale_source = excluded_root / "radv_dumps_6000_old"
        stale_source.mkdir()
        excluded_before = snapshot_radv_dumps(excluded_root)
        wrong_source = excluded_root / "radv_dumps_6001_new"
        wrong_source.mkdir()
        excluded = collect_radv_hang_trace(
            excluded_before, 6000, temp / "excluded-incident", wait_secs=0
        )
        assert excluded.status == "UNKNOWN"
        excluded_report = "\n".join(excluded.report)
        assert "excluded stale=1" in excluded_report
        assert "excluded wrong-pid=1" in excluded_report

        partial_root = temp / "partial-root"
        partial_root.mkdir()
        partial_before = snapshot_radv_dumps(partial_root)
        partial_source = partial_root / "radv_dumps_7000_new"
        partial_source.mkdir()
        (partial_source / "pipeline.log").write_text("no trace", encoding="utf-8")
        partial = collect_radv_hang_trace(
            partial_before, 7000, temp / "partial-incident", wait_secs=0
        )
        assert partial.status == "PARTIAL"
        assert "trace.log: UNKNOWN (missing" in "\n".join(partial.report)

        denied_root = temp / "denied-root"
        denied_root.mkdir()
        denied_before = snapshot_radv_dumps(denied_root)
        denied_source = denied_root / "radv_dumps_8000_new"
        denied_source.mkdir()
        denied_trace = denied_source / "trace.log"
        denied_trace.write_text(trace_text, encoding="utf-8")
        real_open = Path.open

        def deny_trace(path: Path, *args, **kwargs):
            if path == denied_trace:
                raise PermissionError("planted denied trace")
            return real_open(path, *args, **kwargs)

        with mock.patch.object(Path, "open", deny_trace):
            denied = collect_radv_hang_trace(
                denied_before, 8000, temp / "denied-incident", wait_secs=0
            )
        assert denied.status == "PERMISSION-DENIED"
        assert denied.artifact is None

        with mock.patch.object(
            os, "replace", side_effect=PermissionError("planted atomic publish denial")
        ):
            denied_publish_status, denied_publish_report = _copy_evidence(
                [positive_source],
                temp / "denied-publish.radv",
                max_files=8,
                max_bytes=4096,
                read_secs=1.0,
            )
        assert denied_publish_status == "PERMISSION-DENIED"
        assert "atomic artifact publish failed" in "\n".join(denied_publish_report)
        assert not (temp / "denied-publish.radv").exists()

        bounded_root = temp / "bounded-root"
        bounded_root.mkdir()
        bounded_before = snapshot_radv_dumps(bounded_root)
        bounded_source = bounded_root / "radv_dumps_9000_new"
        bounded_source.mkdir()
        (bounded_source / "trace.log").write_text(trace_text, encoding="utf-8")
        (bounded_source / "oversized.bin").write_bytes(b"0123456789")
        bounded = collect_radv_hang_trace(
            bounded_before,
            9000,
            temp / "bounded-incident",
            wait_secs=0,
            max_files=1,
            max_bytes=8,
        )
        assert bounded.status == "PARTIAL"
        bounded_report = "\n".join(bounded.report)
        assert "file cap" in bounded_report or "byte cap" in bounded_report

        timed_root = temp / "timed-root"
        timed_root.mkdir()
        (timed_root / "trace.log").write_text(trace_text, encoding="utf-8")
        with mock.patch.object(time, "monotonic", side_effect=[0.0, 2.0]):
            timed_status, timed_report = _copy_evidence(
                [timed_root],
                temp / "timed-incident.radv",
                max_files=8,
                max_bytes=1024,
                read_secs=1.0,
            )
        assert timed_status == "PARTIAL"
        assert "read-time cap" in "\n".join(timed_report)
        assert not (temp / "timed-incident.radv").exists()

        unavailable = snapshot_radv_dumps(temp / "missing-root")
        assert unavailable.status == "UNAVAILABLE"

        invalid_bounds = collect_radv_hang_trace(
            empty_before,
            5000,
            temp / "invalid-bounds-incident",
            wait_secs=math.inf,
        )
        assert invalid_bounds.status == "UNAVAILABLE"
        assert "finite" in "\n".join(invalid_bounds.report)

        policy_environment = {"SBR_RADV_HANG_DIAG": "1", "RADV_DEBUG": "zerovram"}
        assert configure_radv_hang_environment(policy_environment)
        assert policy_environment["RADV_DEBUG"] == "zerovram,hang"
        assert radv_hang_enabled(policy_environment)
        assert not configure_radv_hang_environment({"SBR_RADV_HANG_DIAG": "0"})
        try:
            configure_radv_hang_environment({"RADV_DEBUG": "hang"})
        except ValueError:
            pass
        else:
            raise AssertionError("unguarded RADV_DEBUG=hang was accepted")
        try:
            configure_radv_hang_environment({"SBR_RADV_HANG_DIAG": "yes"})
        except ValueError:
            pass
        else:
            raise AssertionError("invalid RADV hang diagnostic opt-in was accepted")

        real_iterdir = Path.iterdir

        def deny_root(path: Path):
            if path == temp / "denied-scan-root":
                raise PermissionError("planted scan denial")
            return real_iterdir(path)

        (temp / "denied-scan-root").mkdir()
        with mock.patch.object(Path, "iterdir", deny_root):
            denied_snapshot = snapshot_radv_dumps(temp / "denied-scan-root")
        assert denied_snapshot.status == "PERMISSION-DENIED"

    print("radv-hang-trace selftest PASS")
    print("  exact-child positive reports both CP-reached and CP-not-reached trace IDs")
    print("  no-new, stale and wrong-PID controls remain UNKNOWN")
    print("  missing, denied, unavailable and bounded evidence disagree explicitly")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--selftest", action="store_true")
    args = parser.parse_args()
    if args.selftest:
        return selftest()
    parser.error(
        "no action requested; this module is integrated by the guarded diagnostic lane"
    )


if __name__ == "__main__":
    raise SystemExit(main())

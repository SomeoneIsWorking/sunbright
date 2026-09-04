"""CPU-only controls for the live GPU guard."""

from __future__ import annotations

import contextlib
import io
import json
import os
import signal
import subprocess
import sys
import tempfile
import time
from pathlib import Path
from unittest import mock

import gpu_watch
from gpu_watch import (
    REPO,
    WATCH_BROKEN_RC,
    WATCH_FAULT_RC,
    JournalPump,
    _bounded_reap,
    _flight_analysis,
    _kill_exact_group,
    _static_environment,
    _write_incident,
    _write_minimal_fault,
    atomic_durable_replace,
    is_gpu_fault,
    kernel_realtime_ns,
    run_guarded,
)


def _process_stopped(pid: int) -> bool:
    status = Path(f"/proc/{pid}/status")
    if not status.exists():
        return True
    for line in status.read_text(errors="replace").splitlines():
        if line.startswith("State:"):
            return "Z" in line
    return False


def _wait_for_nonempty_path(path: Path, timeout: float = 3.0) -> None:
    """Wait for a PID marker's payload, not merely the earlier file creation."""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if path.exists() and path.stat().st_size > 0:
            return
        time.sleep(0.01)
    raise AssertionError(f"timed out waiting for populated selftest marker {path}")


def _wait_process_stopped(pid: int, timeout: float = 1.0) -> None:
    """Allow SIGKILL delivery/reparenting to settle before judging the process-tree control."""
    deadline = time.monotonic() + timeout
    while not _process_stopped(pid) and time.monotonic() < deadline:
        time.sleep(0.01)
    if not _process_stopped(pid):
        raise AssertionError(f"guarded descendant {pid} survived process-group SIGKILL")


def selftest() -> int:
    module = gpu_watch
    planted = (
        "2026-08-26T22:48:34.123456789+03:00 kernel: "
        "[drm:gfx_v10_0_priv_reg_irq [amdgpu]] *ERROR* "
        "Illegal register access in command stream"
    )
    planted_feed = (
        planted + "\n"
        "2026-08-26T22:48:34.123456790+03:00 kernel: amdgpu 0000:0b:00.0: "
        "AMDGPU device coredump file has been created\n"
        "2026-08-26T22:48:34.123456790+03:00 kernel: amdgpu 0000:0b:00.0: "
        "ring gfx_0.0.0 timeout\n"
        "2026-08-26T22:48:34.123456791+03:00 kernel: amdgpu 0000:0b:00.0: "
        "Process planted pid 123\n"
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
        no_dump_root = temp / "no-device-coredump"
        no_dump_root.mkdir()
        (no_dump_root / "disabled").write_text("0\n", encoding="ascii")

        with (
            mock.patch.dict(os.environ, {"RADV_DEBUG": "hang"}, clear=True),
            mock.patch.object(subprocess, "Popen") as forbidden_radv_launch,
        ):
            rejected_radv = run_guarded(
                ["must-not-launch"],
                1,
                kernel_command=["must-not-launch-journal"],
                include_flight=False,
                devcoredump_root=no_dump_root,
            )
        assert rejected_radv.returncode == WATCH_BROKEN_RC
        assert "requires the explicit SBR_RADV_HANG_DIAG=1" in (
            rejected_radv.fault_line or ""
        )
        forbidden_radv_launch.assert_not_called()

        clean_fifo = temp / "clean.fifo"
        os.mkfifo(clean_fifo)
        clean_kernel = [
            sys.executable,
            "-u",
            "-c",
            f"import time; print(open({str(clean_fifo)!r}).read(), end=''); time.sleep(30)",
        ]
        clean_child = [
            sys.executable,
            "-u",
            "-c",
            f"open({str(clean_fifo)!r}, 'w').write('kernel: harmless control line\\n')",
        ]
        clean = run_guarded(
            clean_child,
            None,
            incident_dir=temp / "incidents",
            stamp=temp / "stamp",
            kernel_command=clean_kernel,
            include_flight=False,
            devcoredump_root=no_dump_root,
        )
        assert clean.returncode == 0, clean
        assert not (temp / "stamp").exists()

        cpu_signal_incidents = temp / "cpu-signal-incidents"
        cpu_signal_kernel = [
            sys.executable,
            "-c",
            "import time; time.sleep(30)",
        ]
        cpu_signal_script = (
            "import os,pathlib,resource,signal; "
            f"root=pathlib.Path({str(cpu_signal_incidents)!r}); root.mkdir(); "
            "(root/f'session_{os.getpid()}_before-submit.flight.report.txt').touch(); "
            "resource.setrlimit(resource.RLIMIT_CORE,(0,0)); "
            "os.kill(os.getpid(),signal.SIGSEGV)"
        )
        cpu_signal_stderr = io.StringIO()
        with (
            mock.patch.object(
                module,
                "_core_dump_evidence",
                return_value=["status: FOUND (planted exact-PID coredump record)"],
            ),
            contextlib.redirect_stderr(cpu_signal_stderr),
        ):
            cpu_signal = run_guarded(
                [sys.executable, "-c", cpu_signal_script],
                5,
                incident_dir=cpu_signal_incidents,
                stamp=temp / "cpu-signal.stamp",
                kernel_command=cpu_signal_kernel,
                include_flight=False,
                devcoredump_root=no_dump_root,
            )
        assert cpu_signal.returncode == 128 + signal.SIGSEGV, cpu_signal
        assert cpu_signal.fault_line == "SIGSEGV (signal 11)"
        assert cpu_signal.incident_path is not None
        cpu_signal_text = cpu_signal.incident_path.read_text(errors="replace")
        assert "classification: PROCESS SIGNAL (not a detected kernel GPU fault)" in (
            cpu_signal_text
        )
        assert "termination: SIGSEGV (signal 11)" in cpu_signal_text
        assert "shell-compatible exit code: 139" in cpu_signal_text
        assert "status: FOUND (planted exact-PID coredump record)" in cpu_signal_text
        assert "status: EMPTY" in cpu_signal_text
        assert "size=0" in cpu_signal_text
        assert "empty submit-flight sidecar is not GPU-fault evidence" in cpu_signal_text
        assert "CPU/process signal case" in cpu_signal_stderr.getvalue()
        assert "not a detected kernel GPU fault" in cpu_signal_stderr.getvalue()
        assert not (temp / "cpu-signal.stamp").exists()

        ordinary_failure_incidents = temp / "ordinary-failure-incidents"
        ordinary_failure_stderr = io.StringIO()
        with contextlib.redirect_stderr(ordinary_failure_stderr):
            ordinary_failure = run_guarded(
                [sys.executable, "-c", "raise SystemExit(11)"],
                5,
                incident_dir=ordinary_failure_incidents,
                stamp=temp / "ordinary-failure.stamp",
                kernel_command=[
                    sys.executable,
                    "-c",
                    "import time; time.sleep(30)",
                ],
                include_flight=False,
                devcoredump_root=no_dump_root,
            )
        assert ordinary_failure.returncode == 11, ordinary_failure
        assert ordinary_failure.incident_path is None
        assert not list(ordinary_failure_incidents.glob("cpu_signal_*.txt"))
        assert "CPU/process signal case" not in ordinary_failure_stderr.getvalue()
        assert not (temp / "ordinary-failure.stamp").exists()

        radv_exit_fifo = temp / "radv-exit.fifo"
        os.mkfifo(radv_exit_fifo)
        radv_exit_root = temp / "radv-exit-root"
        radv_exit_root.mkdir()
        radv_exit_incidents = temp / "radv-exit-incidents"
        radv_exit_kernel = [
            sys.executable,
            "-u",
            "-c",
            f"import time; print(open({str(radv_exit_fifo)!r}).read(), end=''); time.sleep(30)",
        ]
        radv_exit_script = (
            "import os,pathlib; "
            f"root=pathlib.Path({str(radv_exit_root)!r}); "
            "dump=root/f'radv_dumps_{os.getpid()}_clean-exit'; dump.mkdir(); "
            "(dump/'trace.log').write_text('Trace ID: a\\nThis trace point was reached by the CP.\\n'"
            "+'Trace ID: b\\nThis trace point was NOT reached by the CP.\\n'); "
            f"open({str(radv_exit_fifo)!r},'w').write('kernel: harmless RADV exit control\\n')"
        )
        with mock.patch.dict(
            os.environ,
            {"RADV_DEBUG": "hang", "SBR_RADV_HANG_DIAG": "1"},
            clear=True,
        ):
            radv_exit = run_guarded(
                [sys.executable, "-c", radv_exit_script],
                5,
                incident_dir=radv_exit_incidents,
                stamp=temp / "radv-exit.stamp",
                kernel_command=radv_exit_kernel,
                include_flight=False,
                devcoredump_root=no_dump_root,
                radv_dump_root=radv_exit_root,
            )
        assert radv_exit.returncode == 0, radv_exit
        radv_exit_reports = list(radv_exit_incidents.glob("radv_*.txt"))
        radv_exit_artifacts = list(radv_exit_incidents.glob("*.radv"))
        assert len(radv_exit_reports) == 1
        assert len(radv_exit_artifacts) == 1
        radv_exit_text = radv_exit_reports[0].read_text(errors="replace")
        assert "terminal outcome: child exit; returncode 0" in radv_exit_text
        assert "status: CAPTURED" in radv_exit_text
        assert f"artifact: {radv_exit_artifacts[0]}" in radv_exit_text

        radv_nonzero_fifo = temp / "radv-nonzero.fifo"
        os.mkfifo(radv_nonzero_fifo)
        radv_nonzero_root = temp / "radv-nonzero-root"
        radv_nonzero_root.mkdir()
        radv_nonzero_incidents = temp / "radv-nonzero-incidents"
        radv_nonzero_kernel = [
            sys.executable,
            "-u",
            "-c",
            f"import time; print(open({str(radv_nonzero_fifo)!r}).read(), end=''); time.sleep(30)",
        ]
        radv_nonzero_script = (
            f"open({str(radv_nonzero_fifo)!r},'w').write('kernel: harmless nonzero control\\n'); "
            "raise SystemExit(7)"
        )
        with mock.patch.dict(
            os.environ,
            {"RADV_DEBUG": "hang", "SBR_RADV_HANG_DIAG": "1"},
            clear=True,
        ):
            radv_nonzero = run_guarded(
                [sys.executable, "-c", radv_nonzero_script],
                5,
                incident_dir=radv_nonzero_incidents,
                stamp=temp / "radv-nonzero.stamp",
                kernel_command=radv_nonzero_kernel,
                include_flight=False,
                devcoredump_root=no_dump_root,
                radv_dump_root=radv_nonzero_root,
            )
        assert radv_nonzero.returncode == 7, radv_nonzero
        radv_nonzero_reports = list(radv_nonzero_incidents.glob("radv_*.txt"))
        assert len(radv_nonzero_reports) == 1
        radv_nonzero_text = radv_nonzero_reports[0].read_text(errors="replace")
        assert "terminal outcome: child exit; returncode 7" in radv_nonzero_text
        assert "status: UNKNOWN" in radv_nonzero_text
        assert "artifact: NONE" in radv_nonzero_text
        assert not list(radv_nonzero_incidents.glob("*.radv"))

        fault_fifo = temp / "fault.fifo"
        os.mkfifo(fault_fifo)
        child_marker = temp / "child.pid"
        fault_devcoredump_root = temp / "fault-device-coredump"
        fault_devcoredump_root.mkdir()
        (fault_devcoredump_root / "disabled").write_text("0\n", encoding="ascii")
        fault_radv_root = temp / "fault-radv"
        fault_radv_root.mkdir()
        fault_dump = b"planted live AMDGPU coredump evidence\n"
        fault_device = temp / "0000:0b:00.0"
        fault_device.mkdir()
        fault_kernel = [
            sys.executable,
            "-u",
            "-c",
            f"import time; print(open({str(fault_fifo)!r}).read(), end=''); time.sleep(30)",
        ]
        fault_script = (
            "import os,pathlib,subprocess,sys,time; "
            "p=subprocess.Popen([sys.executable,'-c','import time; time.sleep(30)']); "
            f"open({str(child_marker)!r},'w').write(str(p.pid)); "
            f"node=pathlib.Path({str(fault_devcoredump_root / 'devcd4')!r}); "
            "node.mkdir(); "
            f"(node/'data').write_bytes({fault_dump!r}); "
            f"(node/'failing_device').symlink_to({str(fault_device)!r}); "
            f"radv=pathlib.Path({str(fault_radv_root)!r})/f'radv_dumps_{{os.getpid()}}_planted'; "
            "radv.mkdir(); "
            "(radv/'trace.log').write_text('Trace ID: a\\nThis trace point was reached by the CP.\\n'"
            "+'Trace ID: b\\nThis trace point was NOT reached by the CP.\\n'); "
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
            mock.patch.object(
                module, "_write_minimal_fault", side_effect=ordered_minimal
            ),
            mock.patch.dict(
                os.environ,
                {"RADV_DEBUG": "hang", "SBR_RADV_HANG_DIAG": "1"},
                clear=False,
            ),
        ):
            fault = run_guarded(
                [sys.executable, "-u", "-c", fault_script],
                5,
                incident_dir=temp / "incidents",
                stamp=temp / "stamp",
                kernel_command=fault_kernel,
                include_flight=False,
                devcoredump_root=fault_devcoredump_root,
                radv_dump_root=fault_radv_root,
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
        assert "--- Linux device-coredump evidence ---" in incident_text
        assert "status: CAPTURED" in incident_text
        assert "correlation: MATCHED kernel PCI device" in incident_text
        assert "--- RADV hang diagnostic evidence ---" in incident_text
        assert "status: CAPTURED" in incident_text
        assert "last CP-reached trace: 0xa" in incident_text
        assert "first CP-not-reached trace: 0xb" in incident_text
        captured_dumps = list((temp / "incidents").glob("*.devcoredump-devcd4.bin"))
        assert len(captured_dumps) == 1
        assert captured_dumps[0].read_bytes() == fault_dump
        captured_radv = list((temp / "incidents").glob("*.radv"))
        assert len(captured_radv) == 1
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
            flight_dir,
            temp / "flight.stamp",
            4242,
            planted,
            expected_real_ns,
            "gpu_fault",
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
        assert fsync_control.call_count >= 2, (
            "file and new directory entry were not both fsynced"
        )
        assert durable_control.read_text() == "durable-control\n"
        with mock.patch.object(
            os, "replace", side_effect=OSError("planted replace failure")
        ):
            try:
                atomic_durable_replace(durable_control, "must-not-truncate\n")
            except OSError:
                pass
            else:
                raise AssertionError("planted atomic replacement failure was swallowed")
        assert durable_control.read_text() == "durable-control\n"

        exact_snapshot = {
            str(fake_flight.resolve()): (
                fake_flight.stat().st_ino,
                fake_flight.stat().st_size,
                fake_flight.stat().st_mtime_ns,
            )
        }
        assert "no new/changed" in _flight_analysis(
            4242, expected_real_ns, exact_snapshot, flight_dir, fake_reader
        )
        with mock.patch.object(
            subprocess,
            "run",
            side_effect=subprocess.TimeoutExpired(["fake-reader"], 15),
        ):
            assert "failed safely" in _flight_analysis(
                4242, expected_real_ns, {}, flight_dir, fake_reader
            )

        with mock.patch.dict(
            os.environ, {"SBR_AUTH_TOKEN": "must-not-leak"}, clear=True
        ):
            static = "\n".join(
                _static_environment(
                    [
                        "control",
                        "SBR_AUTH_TOKEN=argv-must-not-leak",
                        "--password=also-secret",
                    ]
                )
            )
        assert "env SBR_AUTH_TOKEN=<redacted>" in static
        assert "must-not-leak" not in static
        assert "argv-must-not-leak" not in static
        assert "also-secret" not in static
        assert "SBR_AUTH_TOKEN=<redacted>" in static
        assert "--password=<redacted>" in static

        with (
            mock.patch.object(
                module, "current_kernel_cursor", return_value=("anchor", None)
            ),
            mock.patch.object(module, "preflight_reasons", return_value=[]),
            mock.patch.object(
                module, "kernel_lines_after_cursor", return_value=([planted], None)
            ),
            mock.patch.object(subprocess, "Popen") as forbidden_launch,
        ):
            gap = run_guarded(
                ["must-not-launch"],
                1,
                stamp=temp / "gap.stamp",
                devcoredump_root=no_dump_root,
            )
        assert gap.returncode == WATCH_BROKEN_RC
        forbidden_launch.assert_not_called()

        class NeverReaps:
            pid = 99

            @staticmethod
            def wait(timeout):
                raise subprocess.TimeoutExpired("never", timeout)

        assert "did not reap" in (_bounded_reap(NeverReaps(), timeout=0.001) or "")

        radv_timeout_root = temp / "radv-timeout-root"
        radv_timeout_root.mkdir()
        radv_timeout_incidents = temp / "radv-timeout-incidents"
        radv_timeout_journal = [
            sys.executable,
            "-c",
            "import time; time.sleep(30)",
        ]
        with mock.patch.dict(
            os.environ,
            {"RADV_DEBUG": "hang", "SBR_RADV_HANG_DIAG": "1"},
            clear=True,
        ):
            radv_timeout = run_guarded(
                [sys.executable, "-c", "import time; time.sleep(30)"],
                0.05,
                incident_dir=radv_timeout_incidents,
                stamp=temp / "radv-timeout.stamp",
                kernel_command=radv_timeout_journal,
                include_flight=False,
                devcoredump_root=no_dump_root,
                radv_dump_root=radv_timeout_root,
            )
        assert radv_timeout.returncode == 124, radv_timeout
        radv_timeout_reports = list(radv_timeout_incidents.glob("radv_*.txt"))
        assert len(radv_timeout_reports) == 1
        radv_timeout_text = radv_timeout_reports[0].read_text(errors="replace")
        assert "terminal outcome: wall-clock timeout; returncode 124" in radv_timeout_text
        assert "status: UNKNOWN" in radv_timeout_text

        timeout_journal = [sys.executable, "-c", "import time; time.sleep(30)"]
        timeout_order: list[str] = []
        timeout_scans = iter((([], None), ([], None), ([planted], None)))
        real_kill_exact_group = module._kill_exact_group
        real_bounded_reap = module._bounded_reap

        def ordered_timeout_scan(_cursor: str) -> tuple[list[str], str | None]:
            result = next(timeout_scans)
            if result[0] == [planted]:
                timeout_order.append("final-barrier")
            return result

        def ordered_kill(pgid: int) -> None:
            timeout_order.append("kill")
            real_kill_exact_group(pgid)

        def ordered_reap(
            process: subprocess.Popen[str], timeout: float = module.REAP_TIMEOUT_SECS
        ) -> str | None:
            timeout_order.append("reap-attempt")
            return real_bounded_reap(process, timeout)

        with (
            mock.patch.object(
                module, "current_kernel_cursor", return_value=("timeout-anchor", None)
            ),
            mock.patch.object(module, "preflight_reasons", return_value=[]),
            mock.patch.object(
                module,
                "kernel_lines_after_cursor",
                side_effect=ordered_timeout_scan,
            ) as timeout_barriers,
            mock.patch.object(module, "_kill_exact_group", side_effect=ordered_kill),
            mock.patch.object(module, "_bounded_reap", side_effect=ordered_reap),
            mock.patch.object(
                module, "_journal_command", return_value=timeout_journal
            ),
            mock.patch.object(
                module, "_collect_fault_context", return_value=[planted]
            ),
        ):
            timeout_fault = run_guarded(
                [sys.executable, "-c", "import time; time.sleep(30)"],
                0.05,
                incident_dir=temp / "timeout-fault",
                stamp=temp / "timeout-fault.stamp",
                include_flight=False,
                devcoredump_root=no_dump_root,
            )
        assert timeout_fault.returncode == WATCH_FAULT_RC, timeout_fault
        assert timeout_fault.incident_path is not None
        assert timeout_fault.incident_path.is_file()
        assert timeout_barriers.call_count == 3
        assert timeout_order[:3] == ["kill", "reap-attempt", "final-barrier"]
        timeout_fault_text = timeout_fault.incident_path.read_text(errors="replace")
        assert "Illegal register access" in timeout_fault_text
        assert (temp / "timeout-fault.stamp").is_file()

        fast_fifo = temp / "fast.fifo"
        os.mkfifo(fast_fifo)
        fast_kernel = [
            sys.executable,
            "-u",
            "-c",
            f"import time; print(open({str(fast_fifo)!r}).read(), end=''); time.sleep(30)",
        ]
        fast_child = [
            sys.executable,
            "-c",
            f"open({str(fast_fifo)!r},'w').write({planted_feed!r})",
        ]
        fast = run_guarded(
            fast_child,
            5,
            incident_dir=temp / "fast",
            stamp=temp / "fast.stamp",
            kernel_command=fast_kernel,
            include_flight=False,
            devcoredump_root=no_dump_root,
        )
        assert fast.returncode == WATCH_FAULT_RC, fast

        orphan_fifo = temp / "orphan.fifo"
        os.mkfifo(orphan_fifo)
        orphan_marker = temp / "orphan.pid"
        orphan_kernel = [
            sys.executable,
            "-u",
            "-c",
            f"import time; print(open({str(orphan_fifo)!r}).read(), end=''); time.sleep(30)",
        ]
        orphan_script = (
            "import subprocess,sys; "
            "p=subprocess.Popen([sys.executable,'-c','import time; time.sleep(30)']); "
            f"open({str(orphan_marker)!r},'w').write(str(p.pid)); "
            f"open({str(orphan_fifo)!r},'w').write('kernel: harmless control\\n')"
        )
        orphan = run_guarded(
            [sys.executable, "-c", orphan_script],
            5,
            incident_dir=temp / "orphan",
            stamp=temp / "orphan.stamp",
            kernel_command=orphan_kernel,
            include_flight=False,
            devcoredump_root=no_dump_root,
        )
        assert orphan.returncode == 0, orphan
        _wait_process_stopped(int(orphan_marker.read_text()))

        failed_fifo = temp / "failed-orphan.fifo"
        os.mkfifo(failed_fifo)
        failed_marker = temp / "failed-orphan.pid"
        failed_kernel = [
            sys.executable,
            "-u",
            "-c",
            f"print(open({str(failed_fifo)!r}).read(), end='')",
        ]
        failed_script = (
            "import subprocess,sys; "
            "p=subprocess.Popen([sys.executable,'-c','import time; time.sleep(30)']); "
            f"open({str(failed_marker)!r},'w').write(str(p.pid)); "
            f"open({str(failed_fifo)!r},'w').write('kernel: harmless then watcher EOF\\n')"
        )
        failed = run_guarded(
            [sys.executable, "-c", failed_script],
            5,
            incident_dir=temp / "failed-orphan",
            stamp=temp / "failed-orphan.stamp",
            kernel_command=failed_kernel,
            include_flight=False,
            devcoredump_root=no_dump_root,
        )
        assert failed.returncode == WATCH_BROKEN_RC, failed
        _wait_process_stopped(int(failed_marker.read_text()))

        broken_kernel = [sys.executable, "-c", "raise SystemExit(7)"]
        broken = run_guarded(
            [sys.executable, "-c", "import time; time.sleep(30)"],
            5,
            incident_dir=temp / "broken",
            stamp=temp / "broken.stamp",
            kernel_command=broken_kernel,
            include_flight=False,
            devcoredump_root=no_dump_root,
        )
        assert broken.returncode == WATCH_BROKEN_RC, broken
        assert (temp / "broken.stamp").is_file()

        early_child_marker = temp / "early-signal-child"
        early_signal_kernel = [
            sys.executable,
            "-c",
            (
                "import os,signal,time; os.kill(os.getppid(), signal.SIGTERM); "
                "print('signal-sent', flush=True); time.sleep(30)"
            ),
        ]
        real_journal_start = JournalPump.start

        def synchronized_journal_start(pump: JournalPump) -> None:
            real_journal_start(pump)
            signal_line = pump.lines.get(timeout=1)
            assert signal_line == "signal-sent"
            pump.lines.put(signal_line)

        with mock.patch.object(JournalPump, "start", synchronized_journal_start):
            early_signal = run_guarded(
                [
                    sys.executable,
                    "-c",
                    f"open({str(early_child_marker)!r},'w').write('launched')",
                ],
                5,
                incident_dir=temp / "early-signal",
                stamp=temp / "early-signal.stamp",
                kernel_command=early_signal_kernel,
                include_flight=False,
                devcoredump_root=no_dump_root,
            )
        assert early_signal.returncode == 143
        assert not early_child_marker.exists(), (
            "signal before child creation still launched child"
        )

        signal_child_marker = temp / "signal-child.pids"
        signal_journal_marker = temp / "signal-journal.pid"
        guard_pid = os.fork()
        if guard_pid == 0:
            signal_kernel = [
                sys.executable,
                "-c",
                (
                    f"import os,time; open({str(signal_journal_marker)!r},'w').write(str(os.getpid())); "
                    "time.sleep(30)"
                ),
            ]
            signal_script = (
                "import os,subprocess,sys,time; time.sleep(.1); "
                "p=subprocess.Popen([sys.executable,'-c','import time; time.sleep(30)']); "
                f"open({str(signal_child_marker)!r},'w').write(str(os.getpid())+' '+str(p.pid)); "
                "time.sleep(30)"
            )
            result = run_guarded(
                [sys.executable, "-c", signal_script],
                5,
                incident_dir=temp / "signal",
                stamp=temp / "signal.stamp",
                kernel_command=signal_kernel,
                include_flight=False,
                devcoredump_root=no_dump_root,
            )
            os._exit(result.returncode)
        _wait_for_nonempty_path(signal_child_marker)
        _wait_for_nonempty_path(signal_journal_marker)
        leader_pid, signal_descendant = map(
            int, signal_child_marker.read_text().split()
        )
        signal_journal_pid = int(signal_journal_marker.read_text())
        os.kill(guard_pid, signal.SIGTERM)
        _waited, guard_status = os.waitpid(guard_pid, 0)
        assert os.waitstatus_to_exitcode(guard_status) == 143
        _wait_process_stopped(leader_pid)
        _wait_process_stopped(signal_descendant)
        _wait_process_stopped(signal_journal_pid)

    print("gpu-watch selftest PASS")
    print(
        "  known-negative harmless journal line: command exits normally, no cooldown stamp"
    )
    print("  unlimited interactive guard accepts a clean child exit")
    print(
        "  SIGSEGV becomes exit 139 with a CPU/core incident; ordinary exit 11 is not misclassified"
    )
    print(
        "  known-positive illegal-register line: exact group killed before durable incident writes"
    )
    print(
        "  nanosecond timestamp reaches fake flight reader and emits CAUSAL-WINDOW output"
    )
    print("  durable-write control observes file and parent-directory fsync")
    print("  interrupted atomic enrichment preserves the prior minimal incident")
    print(
        "  stale-flight, reader-timeout, secret-redaction and preflight-gap controls pass"
    )
    print("  live fault incident preserves its fixture-backed device coredump")
    print("  ambient RADV_DEBUG=hang without the explicit opt-in refuses before launch")
    print("  bounded-reap control reports timeout without blocking")
    print("  timeout final barrier converts a late kernel fault into a durable incident")
    print(
        "  clean, nonzero and timeout RADV terminals persist CAPTURED/UNKNOWN reports"
    )
    print("  fast-exit fault drains; clean leader exit kills its surviving descendant")
    print("  watcher-loss controls kill live leader and post-leader descendant")
    print(
        "  early SIGTERM forbids child launch; live SIGTERM kills leader, descendant and follower"
    )
    return 0

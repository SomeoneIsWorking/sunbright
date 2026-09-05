"""Portable positive/negative controls through the actual DLL deployment owner."""

import subprocess
import sys
import unittest
from pathlib import Path
from tempfile import TemporaryDirectory

from runtime_dependencies import deploy


class RuntimeDependenciesTest(unittest.TestCase):
    def setUp(self) -> None:
        scratch = Path(__file__).resolve().parents[1] / "scratch" / "runtime-deployment"
        scratch.mkdir(parents=True, exist_ok=True)
        self.temporary = TemporaryDirectory(dir=scratch)
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.source = self.root / "SDL3.dll"
        self.source.write_bytes(b"synthetic redistributable DLL bytes")
        self.destination = self.root / "executable"
        self.destination.mkdir()
        self.manifest = self.root / "runtime.txt"
        self.manifest.write_text(f"{self.source}\n", encoding="utf-8")

    def run_owner(self, *, check: bool) -> int:
        return deploy(self.manifest, self.destination, [self.source], check=check)

    def test_missing_deployment_refuses_then_build_repairs(self) -> None:
        with self.assertRaisesRegex(ValueError, "missing or stale"):
            self.run_owner(check=True)
        self.assertEqual(self.run_owner(check=False), 1)
        self.assertEqual(self.run_owner(check=True), 1)
        deployed = self.destination / self.source.name
        self.assertEqual(deployed.read_bytes(), self.source.read_bytes())
        unchanged_time = deployed.stat().st_mtime_ns
        self.run_owner(check=False)
        self.assertEqual(deployed.stat().st_mtime_ns, unchanged_time)

    def test_stale_deployment_refuses_then_repairs(self) -> None:
        (self.destination / self.source.name).write_bytes(b"wrong revision")
        with self.assertRaisesRegex(ValueError, "missing or stale"):
            self.run_owner(check=True)
        self.run_owner(check=False)
        self.assertEqual(self.run_owner(check=True), 1)

    def test_missing_required_metadata_refuses(self) -> None:
        self.manifest.write_text("", encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "required imported DLL absent"):
            self.run_owner(check=False)

    def test_missing_source_refuses(self) -> None:
        self.source.unlink()
        with self.assertRaisesRegex(ValueError, "invalid runtime DLL source"):
            self.run_owner(check=False)

    def test_colliding_metadata_refuses(self) -> None:
        self.manifest.write_text(f"{self.source}\n{self.source}\n", encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "basename collision"):
            self.run_owner(check=False)

    def test_static_link_requires_no_dlls(self) -> None:
        self.manifest.write_text("", encoding="utf-8")
        self.assertEqual(deploy(self.manifest, self.destination, [], check=True), 0)

    def test_cmake_windows_metadata_and_registered_check(self) -> None:
        repo = Path(__file__).resolve().parents[1]
        build = repo / "build" / "runtime-dependencies-fixture"
        subprocess.run(
            [
                "cmake",
                "-S",
                str(repo / "tools/fixtures/runtime-dependencies"),
                "-B",
                str(build),
                "-G",
                "Ninja",
                f"-DPython3_EXECUTABLE={sys.executable}",
            ],
            check=True,
            capture_output=True,
            text=True,
        )
        destination = build / "bin"
        (destination / "SDL3.dll").unlink(missing_ok=True)
        command = [
            "ctest",
            "--test-dir",
            str(build),
            "-R",
            "^fixture_runtime$",
            "--output-on-failure",
        ]
        absent = subprocess.run(
            command, check=False, capture_output=True, text=True, timeout=40
        )
        self.assertNotEqual(absent.returncode, 0)
        self.assertIn("runtime DLL missing or stale", absent.stdout)
        manifest = build / "runtime" / "Debug" / "fixture.txt"
        source = build / "dependency" / "SDL3.dll"
        self.assertEqual(deploy(manifest, destination, [source], check=False), 1)
        present = subprocess.run(
            command, check=False, capture_output=True, text=True, timeout=40
        )
        self.assertEqual(present.returncode, 0, present.stdout + present.stderr)

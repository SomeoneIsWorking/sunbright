"""Exercise the quality owner's compile database indexing and candidate matching."""

import json
import ntpath
import posixpath
import unittest
from pathlib import Path
from tempfile import TemporaryDirectory
from types import ModuleType
from unittest.mock import patch

from cpp_quality import compile_databases, matching_commands


class CompileDatabasePathsTest(unittest.TestCase):
    def setUp(self) -> None:
        scratch = Path(__file__).resolve().parents[1] / "scratch" / "cpp-quality"
        scratch.mkdir(parents=True, exist_ok=True)
        self.temporary = TemporaryDirectory(dir=scratch)
        self.addCleanup(self.temporary.cleanup)
        self.build = Path(self.temporary.name)

    def match(
        self,
        entries: list[dict[str, str]],
        candidates: list[str],
        root: str,
        path_module: ModuleType,
    ) -> dict[str, tuple[dict[str, object], Path]]:
        (self.build / "compile_commands.json").write_text(
            json.dumps(entries), encoding="utf-8"
        )
        # These synthetic drives/shares test path grammar, not real filesystem or network access.
        with patch.object(path_module, "realpath", path_module.abspath):
            commands = compile_databases(
                build_directories=(self.build,), path_module=path_module
            )
            return matching_commands(
                candidates, commands, root=root, path_module=path_module
            )

    def test_windows_separators_drive_case_and_command_preserved(self) -> None:
        entry = {
            "directory": "D:/checkout/build",
            "file": r"d:\CHECKOUT\native-render\src\frame.cpp",
            "command": "clang++ -DIDENTITY=42 -c frame.cpp",
        }
        candidate = "native-render/src/frame.cpp"
        matched = self.match([entry], [candidate], r"D:\checkout", ntpath)
        self.assertEqual(matched, {candidate: (entry, self.build)})

    def test_relative_file_uses_command_directory(self) -> None:
        entry = {
            "directory": "D:/checkout/build",
            "file": "../native-render/src/frame.cpp",
            "command": "clang++ -c ../native-render/src/frame.cpp",
        }
        candidate = r"native-render\src\frame.cpp"
        self.assertEqual(
            self.match([entry], [candidate], "D:/checkout", ntpath),
            {candidate: (entry, self.build)},
        )

    def test_distinct_directories_and_drives_do_not_match(self) -> None:
        entries = [
            {"directory": "D:/checkout/build", "file": "D:/checkout/other/frame.cpp"},
            {
                "directory": "E:/checkout/build",
                "file": "E:/checkout/native-render/src/frame.cpp",
            },
            {
                "directory": "D:/external/build",
                "file": "D:/external/native-render/src/frame.cpp",
            },
        ]
        self.assertEqual(
            self.match(entries, ["native-render/src/frame.cpp"], "D:/checkout", ntpath),
            {},
        )

    def test_windows_unc_paths_preserve_share_identity(self) -> None:
        entry = {
            "directory": r"\\server\share\checkout\build",
            "file": "../src/frame.cpp",
        }
        self.assertIn(
            "src/frame.cpp",
            self.match([entry], ["src/frame.cpp"], "//SERVER/share/checkout", ntpath),
        )
        self.assertEqual(
            self.match([entry], ["src/frame.cpp"], "//server/other/checkout", ntpath),
            {},
        )

    def test_posix_case_remains_distinct(self) -> None:
        root = "/checkout"
        entry = {"directory": root, "file": "src/Frame.cpp"}
        candidates = ["src/Frame.cpp", "src/frame.cpp"]
        self.assertEqual(
            self.match([entry], candidates, root, posixpath),
            {"src/Frame.cpp": (entry, self.build)},
        )

    def test_relative_directory_refuses(self) -> None:
        with self.assertRaisesRegex(ValueError, "directory must be absolute"):
            self.match(
                [{"directory": "build", "file": "../src/frame.cpp"}],
                ["src/frame.cpp"],
                "D:/checkout",
                ntpath,
            )

    def test_native_database_matches_real_files(self) -> None:
        source = self.build / "src" / "frame.cpp"
        source.parent.mkdir()
        source.write_text("// redistributable path fixture\n", encoding="utf-8")
        entry = {
            "directory": str(self.build),
            "file": "src/frame.cpp",
            "command": "preserved",
        }
        (self.build / "compile_commands.json").write_text(
            json.dumps([entry]), encoding="utf-8"
        )
        commands = compile_databases(build_directories=(self.build,))
        selected = matching_commands(
            ["src/frame.cpp", "other/frame.cpp"], commands, root=str(self.build)
        )
        self.assertEqual(selected, {"src/frame.cpp": (entry, self.build)})

#!/usr/bin/env python3
"""Produce `scratch/bin/sms.dol` from the user's own retail disc, on demand.

WHY THIS EXISTS
---------------
Every gcnport boot run takes two inputs: the disc image the user supplies through `.env`, and
GMSE01's `main.dol` as a flat file. The disc has an owner and a documented path. The DOL did not:
it was extracted by hand once, months ago, and lived in gitignored `scratch/` with no recorded
producer. `scratch_gc.py --apply` removed it -- correctly, it was older than the retention window --
and three runs then died on `cannot open DOL image 'scratch/bin/sms.dol'` with nothing in the
repository that said how to get it back.

A run input with no reproducible producer is a workflow defect, so this is the producer. It reads
the disc with the Dolphin fork's own `dolphin-tool`, which is the authoritative reader for the
compressed formats the user's library is in, and validates what comes out before handing it over.

WHAT IT REFUSES
---------------
Naming, in each case, exactly what it looked for: a disc path that `.env` and the environment both
failed to supply, a `dolphin-tool` that has not been built, an extraction that produced nothing,
and bytes that are not a self-consistent DOL. It never falls back to a stale file it cannot
validate, because "the run used yesterday's DOL" and "the run used the disc's DOL" must not look
the same from the outside.

Usage:
    python3 tools/disc/main_dol.py                    # ensure it exists, print its path
    python3 tools/disc/main_dol.py --force            # re-extract even if it is already there
    python3 tools/disc/main_dol.py --disc PATH        # a disc other than the one .env names
"""

from __future__ import annotations

import argparse
import os
import subprocess
import sys
import tempfile
from collections.abc import Callable, Mapping, Sequence
from dataclasses import dataclass
from pathlib import Path

SELFTEST_REQUIREMENTS: tuple[str, ...] = ()

REPO_ROOT = Path(__file__).resolve().parents[2]
DISC_ENVIRONMENT_NAME = "SUNBRIGHT_ROM"
DEFAULT_OUTPUT = Path("scratch/bin/sms.dol")
DOLPHIN_TOOL = Path("extern/dolphin_fork/build/Binaries/dolphin-tool")
VOLUME_PATH = "sys/main.dol"

# The DOL header's own shape: seven text sections then eleven data ones, each with a file offset, a
# load address and a size held in three parallel tables, and the entry point at the end.
TEXT_SECTIONS = 7
DATA_SECTIONS = 11
TEXT_OFFSETS = 0x00
DATA_OFFSETS = 0x1C
TEXT_ADDRESSES = 0x48
DATA_ADDRESSES = 0x64
TEXT_SIZES = 0x90
DATA_SIZES = 0xAC
ENTRY_POINT = 0xE0
HEADER_SIZE = 0x100


class ExtractionRefused(Exception):
    """A named input was missing or unusable. The message says which, and where it was looked for."""


@dataclass(frozen=True)
class DolSection:
    file_offset: int
    address: int
    size: int


@dataclass(frozen=True)
class DolSummary:
    sections: tuple[DolSection, ...]
    entry_point: int

    @property
    def load_address(self) -> int:
        return min(section.address for section in self.sections)

    @property
    def end_address(self) -> int:
        return max(section.address + section.size for section in self.sections)


CommandRunner = Callable[[Sequence[str]], subprocess.CompletedProcess]


def run_command(command: Sequence[str]) -> subprocess.CompletedProcess:
    return subprocess.run(command, capture_output=True, text=True, check=False)


def _environment_file_value(path: Path, name: str) -> str | None:
    """The value `name` is given in a shell-sourced `.env`, or None.

    Only the assignment form the file actually uses is understood; anything else is left alone
    rather than guessed at, so a file this cannot read reports "not found" and the caller names it.
    """
    if not path.is_file():
        return None
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        stripped = line.strip()
        if stripped.startswith("#") or "=" not in stripped:
            continue
        key, _, value = stripped.partition("=")
        if key.strip() != name:
            continue
        value = value.strip()
        if len(value) >= 2 and value[0] == value[-1] and value[0] in "\"'":
            value = value[1:-1]
        return value
    return None


def resolve_disc_path(
    explicit: str | None,
    environment: Mapping[str, str],
    repo_root: Path,
) -> Path:
    environment_file = repo_root / ".env"
    candidates = (
        ("--disc", explicit),
        (f"${DISC_ENVIRONMENT_NAME}", environment.get(DISC_ENVIRONMENT_NAME)),
        (
            f"{environment_file}'s {DISC_ENVIRONMENT_NAME}",
            _environment_file_value(environment_file, DISC_ENVIRONMENT_NAME),
        ),
    )
    named = [(source, value) for source, value in candidates if value]
    if not named:
        tried = ", ".join(source for source, _ in candidates)
        raise ExtractionRefused(f"no disc image named by any of: {tried}")
    source, value = named[0]
    disc = Path(value).expanduser()
    if not disc.is_file():
        raise ExtractionRefused(
            f"{source} names '{disc}', which is not a readable file"
        )
    return disc


def resolve_dolphin_tool(repo_root: Path) -> Path:
    tool = repo_root / DOLPHIN_TOOL
    if not os.access(tool, os.X_OK):
        raise ExtractionRefused(
            f"no executable dolphin-tool at '{tool}'; build the fork before extracting"
        )
    return tool


def _big_endian(data: bytes, offset: int) -> int:
    return int.from_bytes(data[offset : offset + 4], "big")


def summarise_dol(data: bytes, origin: str) -> DolSummary:
    """The sections a DOL declares, refusing anything that is not self-consistent.

    This is validation, not loading: it exists so a truncated extraction, a partially written file
    or an entirely different file cannot reach a boot run looking like a DOL.
    """
    if len(data) < HEADER_SIZE:
        raise ExtractionRefused(
            f"{origin} is {len(data)} bytes, smaller than a DOL's {HEADER_SIZE}-byte header"
        )
    sections: list[DolSection] = []
    tables = (
        (TEXT_SECTIONS, TEXT_OFFSETS, TEXT_ADDRESSES, TEXT_SIZES),
        (DATA_SECTIONS, DATA_OFFSETS, DATA_ADDRESSES, DATA_SIZES),
    )
    for count, offsets, addresses, sizes in tables:
        for index in range(count):
            size = _big_endian(data, sizes + 4 * index)
            if size == 0:
                continue
            section = DolSection(
                file_offset=_big_endian(data, offsets + 4 * index),
                address=_big_endian(data, addresses + 4 * index),
                size=size,
            )
            if section.file_offset + section.size > len(data):
                raise ExtractionRefused(
                    f"{origin}: a section at file offset {section.file_offset:#x} of "
                    f"{section.size} bytes runs past the file's {len(data)} bytes"
                )
            sections.append(section)
    if not sections:
        raise ExtractionRefused(f"{origin} declares no non-empty DOL sections")
    summary = DolSummary(tuple(sections), _big_endian(data, ENTRY_POINT))
    if not summary.load_address <= summary.entry_point < summary.end_address:
        raise ExtractionRefused(
            f"{origin}: entry point {summary.entry_point:#010x} lies outside the sections it "
            f"loads, {summary.load_address:#010x}..{summary.end_address:#010x}"
        )
    return summary


def extract_main_dol(
    disc: Path,
    destination: Path,
    *,
    tool: Path,
    run: CommandRunner = run_command,
) -> DolSummary:
    """Extract the disc's `sys/main.dol` to `destination`, replacing it only once it validates."""
    destination.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=destination.parent) as staging_name:
        staging = Path(staging_name)
        command = [
            str(tool),
            "extract",
            "--input",
            str(disc),
            "--output",
            str(staging),
            "--single",
            VOLUME_PATH,
            "--gameonly",
            "--quiet",
        ]
        completed = run(command)
        if completed.returncode != 0:
            detail = (completed.stderr or completed.stdout or "").strip()
            raise ExtractionRefused(
                f"dolphin-tool exited {completed.returncode} extracting {VOLUME_PATH} from "
                f"'{disc}'{': ' + detail if detail else ''}"
            )
        produced = [path for path in staging.rglob("*") if path.is_file()]
        if len(produced) != 1:
            raise ExtractionRefused(
                f"dolphin-tool produced {len(produced)} files extracting {VOLUME_PATH} from "
                f"'{disc}', expected exactly one"
            )
        data = produced[0].read_bytes()
        summary = summarise_dol(data, f"{VOLUME_PATH} of '{disc}'")
        destination.write_bytes(data)
    return summary


def ensure_main_dol(
    *,
    disc: Path,
    destination: Path,
    tool: Path,
    force: bool,
    run: CommandRunner = run_command,
) -> tuple[DolSummary, bool]:
    """(summary, extracted). An existing file is reused only if it still validates as a DOL."""
    if not force and destination.is_file():
        try:
            return summarise_dol(destination.read_bytes(), f"'{destination}'"), False
        except ExtractionRefused:
            pass
    return extract_main_dol(disc, destination, tool=tool, run=run), True


def _selftest_dol(section_size: int = 0x40, entry_offset: int = 0x10) -> bytes:
    header = bytearray(HEADER_SIZE)
    header[TEXT_OFFSETS : TEXT_OFFSETS + 4] = HEADER_SIZE.to_bytes(4, "big")
    header[TEXT_ADDRESSES : TEXT_ADDRESSES + 4] = (0x80003100).to_bytes(4, "big")
    header[TEXT_SIZES : TEXT_SIZES + 4] = section_size.to_bytes(4, "big")
    header[ENTRY_POINT : ENTRY_POINT + 4] = (0x80003100 + entry_offset).to_bytes(
        4, "big"
    )
    return bytes(header) + bytes(section_size)


def selftest() -> int:
    """Both answers, on cases whose answer is forced: a DOL that must validate, and four that must not."""
    good = summarise_dol(_selftest_dol(), "selftest")
    assert good.load_address == 0x80003100, good
    assert good.entry_point == 0x80003110, good
    assert good.end_address == 0x80003140, good

    def refuses(name: str, thunk: Callable[[], object]) -> None:
        try:
            thunk()
        except ExtractionRefused:
            return
        raise AssertionError(f"selftest: {name} was accepted and must not be")

    refuses(
        "a file shorter than the header", lambda: summarise_dol(b"\0" * 16, "selftest")
    )
    refuses(
        "a header with no sections",
        lambda: summarise_dol(bytes(HEADER_SIZE), "selftest"),
    )
    refuses(
        "a section running past the file",
        lambda: summarise_dol(_selftest_dol()[:-8], "selftest"),
    )
    refuses(
        "an entry point outside the loaded sections",
        lambda: summarise_dol(_selftest_dol(entry_offset=0x400), "selftest"),
    )
    refuses(
        "no disc named anywhere",
        lambda: resolve_disc_path(
            None, {}, Path(tempfile.gettempdir()) / "sunbright-absent"
        ),
    )
    refuses(
        "a disc path that is not a file",
        lambda: resolve_disc_path("/nonexistent/disc.rvz", {}, REPO_ROOT),
    )

    with tempfile.TemporaryDirectory() as workspace:
        root = Path(workspace)
        (root / ".env").write_text(f'{DISC_ENVIRONMENT_NAME}="{root / "disc.rvz"}"\n')
        (root / "disc.rvz").write_bytes(b"not really a disc")
        assert resolve_disc_path(None, {}, root) == root / "disc.rvz"
        assert resolve_disc_path(
            None, {DISC_ENVIRONMENT_NAME: str(root / "disc.rvz")}, root
        )

        def silent_runner(_command: Sequence[str]) -> subprocess.CompletedProcess:
            return subprocess.CompletedProcess(_command, 0, "", "")

        refuses(
            "an extraction that produced no file",
            lambda: extract_main_dol(
                root / "disc.rvz",
                root / "out" / "sms.dol",
                tool=Path("/bin/true"),
                run=silent_runner,
            ),
        )

        def writing_runner(command: Sequence[str]) -> subprocess.CompletedProcess:
            output = Path(command[command.index("--output") + 1])
            (output / "main.dol").write_bytes(_selftest_dol())
            return subprocess.CompletedProcess(command, 0, "", "")

        destination = root / "out" / "sms.dol"
        summary, extracted = ensure_main_dol(
            disc=root / "disc.rvz",
            destination=destination,
            tool=Path("/bin/true"),
            force=False,
            run=writing_runner,
        )
        assert extracted and summary.entry_point == 0x80003110
        assert destination.read_bytes() == _selftest_dol()
        _, again = ensure_main_dol(
            disc=root / "disc.rvz",
            destination=destination,
            tool=Path("/bin/true"),
            force=False,
            run=writing_runner,
        )
        assert not again, "a valid DOL already in place must not be extracted again"
        destination.write_bytes(b"truncated")
        _, repaired = ensure_main_dol(
            disc=root / "disc.rvz",
            destination=destination,
            tool=Path("/bin/true"),
            force=False,
            run=writing_runner,
        )
        assert repaired, "a file that no longer validates must be extracted again"

    print("main_dol selftest PASS")
    return 0


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--disc", help=f"disc image; defaults to ${DISC_ENVIRONMENT_NAME}"
    )
    parser.add_argument("--output", default=str(DEFAULT_OUTPUT), type=str)
    parser.add_argument(
        "--force", action="store_true", help="re-extract even if present"
    )
    parser.add_argument("--selftest", action="store_true")
    arguments = parser.parse_args(argv)
    if arguments.selftest:
        return selftest()

    destination = Path(arguments.output)
    if not destination.is_absolute():
        destination = REPO_ROOT / destination
    try:
        tool = resolve_dolphin_tool(REPO_ROOT)
        disc = resolve_disc_path(arguments.disc, os.environ, REPO_ROOT)
        summary, extracted = ensure_main_dol(
            disc=disc, destination=destination, tool=tool, force=arguments.force
        )
    except ExtractionRefused as refusal:
        print(f"main_dol: {refusal}", file=sys.stderr)
        return 1
    print(
        f"main_dol: {'extracted' if extracted else 'reused'} {destination} "
        f"({destination.stat().st_size} bytes, {len(summary.sections)} sections, "
        f"{summary.load_address:#010x}..{summary.end_address:#010x}, "
        f"entry {summary.entry_point:#010x})"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))

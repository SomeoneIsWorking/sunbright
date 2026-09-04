"""Typed Sunbright launcher configuration.

This is the only launcher module allowed to ingest command-line values. Process
environment and persisted settings will be added here when gcnport exists; product
owners will receive an immutable value instead of reading them independently.
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass
from pathlib import Path

HELP_TEXT = """Usage: ./run.sh [--rom PATH]

Sunbright currently has no gameplay executable. The launcher accepts and validates
its stable CLI shape, then refuses until shared gcnport embeds Dolphin's runtime
PowerPC JIT. It never selects an alternate executor.
"""


@dataclass(frozen=True)
class LaunchConfig:
    image_path: Path | None
    show_help: bool


class _Parser(argparse.ArgumentParser):
    def __init__(self) -> None:
        super().__init__(add_help=False)
        self.add_argument("--rom", type=Path)
        self.add_argument("-h", "--help", action="store_true")

    def error(self, message: str) -> None:
        raise ValueError(message)


def parse_launch_config(arguments: list[str]) -> LaunchConfig:
    namespace = _Parser().parse_args(arguments)
    return LaunchConfig(image_path=namespace.rom, show_help=namespace.help)

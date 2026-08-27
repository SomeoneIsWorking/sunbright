"""Shared command-line/environment parsing for Sunbright launchers."""

from __future__ import annotations

import re
from pathlib import Path


REPO = Path(__file__).resolve().parents[2]
ENVIRONMENT_NAME = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")


def apply_environment_assignment(environment: dict[str, str], argument: str) -> None:
    if "=" not in argument:
        raise ValueError(f"expected NAME=VALUE after --, got {argument!r}")
    name, value = argument.split("=", 1)
    if ENVIRONMENT_NAME.fullmatch(name) is None:
        raise ValueError(f"invalid environment assignment: {argument!r}")
    environment[name] = value


def parse_arguments(
    arguments: list[str], inherited: dict[str, str]
) -> tuple[dict[str, str], list[str]]:
    """Parse the development-launcher NAME=VALUE / ``--`` CLI exactly once."""
    environment = dict(inherited)
    runner_args: list[str] = []
    after_separator = False
    for argument in arguments:
        if not after_separator and argument == "--":
            after_separator = True
        elif not after_separator and "=" in argument:
            apply_environment_assignment(environment, argument)
        else:
            runner_args.append(argument)
    return environment, runner_args

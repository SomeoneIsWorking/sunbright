#!/usr/bin/env python3
"""Sunbright's sole launcher boundary.

Gameplay intentionally refuses until the shared gcnport executor embeds Dolphin's
runtime JIT. Keeping this refusal here prevents any stale executable or alternate
native evidence host from silently becoming the product.
"""

from __future__ import annotations

import sys

from config import HELP_TEXT, parse_launch_config

MISSING_EXECUTOR = (
    "Sunbright gameplay is unavailable: shared gcnport does not yet embed "
    "Dolphin's runtime PowerPC JIT"
)


def main(arguments: list[str]) -> int:
    config = parse_launch_config(arguments)
    if config.show_help:
        print(HELP_TEXT)
        return 0
    print(f"sunbright: REFUSING: {MISSING_EXECUTOR}", file=sys.stderr)
    return 2


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))

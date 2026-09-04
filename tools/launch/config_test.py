#!/usr/bin/env python3
"""Focused positive/negative tests for the shipping launch parser."""

from __future__ import annotations

from pathlib import Path

from config import parse_launch_config


def main() -> int:
    parsed = parse_launch_config(["--rom", "game.rvz"])
    assert parsed.image_path == Path("game.rvz")
    assert not parsed.show_help
    assert parse_launch_config(["--help"]).show_help
    try:
        parse_launch_config(["--runtime", "old"])
    except ValueError:
        pass
    else:
        raise AssertionError("retired executor selector was accepted")
    print("launch config: 3 checks passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

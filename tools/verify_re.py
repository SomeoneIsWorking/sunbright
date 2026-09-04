#!/usr/bin/env python3
"""Run Sunbright self-tests that require the user-supplied GMSE01 image."""

from selftest_all import main

if __name__ == "__main__":
    raise SystemExit(main(["--require", "game-image"]))

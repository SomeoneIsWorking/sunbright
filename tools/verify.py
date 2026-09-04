#!/usr/bin/env python3
"""Run Sunbright's canonical asset-free native renderer/tooling gate."""

from pathlib import Path

from verification import gate_steps, run_steps

if __name__ == "__main__":
    raise SystemExit(run_steps(Path(__file__).resolve().parents[1], gate_steps()))

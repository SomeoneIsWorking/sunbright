#!/usr/bin/env python3
"""Compare a rendered frame against an oracle frame, as numbers a repair has to move.

A screenshot next to a screenshot is an opinion. This turns the pair into per-channel and per-region
means and standard deviations, and into the one distinction that says which owner is wrong: a delta
that leaves the standard deviation alone is an ADDITIVE offset -- something is being added to every
pixel, a clear colour, an ambient term, a pass composed in that should not be -- while a delta that
scales it is MULTIPLICATIVE, a gain applied to what was already there. Those are different repairs,
and the mean alone cannot tell them apart.

    tools/render/frame_diff.py scratch/render/frame.png scratch/oracle/frame.png

Either side may be a binary PPM (what `sunbright_gcnport_boot --dump-frame` writes, whatever the
extension says) or anything Pillow reads. Both sides must be the same size; a resized comparison
would be measuring the resampler.

It refuses a degenerate input rather than scoring it. A frame with almost no variance is either a
clear colour nothing drew over or a capture that failed, and diffing against one produces a large,
confident, meaningless number -- one was nearly read as a regression once. "This oracle is blank"
is the answer in that case, and this says so instead of returning a percentage.
"""

from __future__ import annotations

import argparse
import sys
from dataclasses import dataclass
from pathlib import Path

import numpy as np

SELFTEST_REQUIREMENTS: tuple[str, ...] = ()

# Below this standard deviation a frame carries no structure to compare: a flat clear colour, an
# all-black capture, a dump taken before anything drew.
DEGENERATE_STD = 2.0
# How close two standard deviations have to be for a delta to count as leaving structure intact.
ADDITIVE_STD_TOLERANCE = 1.5
CHANNELS = ("R", "G", "B")


def read_frame(path: Path) -> np.ndarray:
    """One frame as HxWx3 uint8, from a binary PPM or any format Pillow reads.

    Pillow is imported here, not at module scope: it is an optional convenience
    for the formats the boot tool does not write, and this repository's locked
    environment does not carry it. Importing it at module scope made the whole
    tool -- including the P6 path that needs nothing but numpy, and the
    self-test -- fail on a clean checkout with `No module named 'PIL'`.
    """
    with open(path, "rb") as handle:
        head = handle.read(2)
    if head == b"P6":
        return _read_ppm(path)
    try:
        from PIL import Image
    except ModuleNotFoundError as exc:
        raise SystemExit(
            f"frame_diff: {path.name} is not a binary PPM, and reading any other "
            f"format needs Pillow, which is not in this project's locked "
            f"environment. Either write the frame as P6 (what "
            f"`sunbright_gcnport_boot --dump-frame` does) or install Pillow "
            f"yourself; the comparison itself needs only numpy."
        ) from exc
    with Image.open(path) as image:
        return np.asarray(image.convert("RGB"), dtype=np.uint8)


def _read_ppm(path: Path) -> np.ndarray:
    data = path.read_bytes()
    fields: list[int] = []
    cursor = 2
    while len(fields) < 3:
        while cursor < len(data) and data[cursor : cursor + 1].isspace():
            cursor += 1
        if data[cursor : cursor + 1] == b"#":
            while cursor < len(data) and data[cursor : cursor + 1] != b"\n":
                cursor += 1
            continue
        start = cursor
        while cursor < len(data) and not data[cursor : cursor + 1].isspace():
            cursor += 1
        fields.append(int(data[start:cursor]))
    cursor += 1  # the single whitespace byte after the maximum value
    width, height, maximum = fields
    if maximum != 255:
        raise ValueError(f"{path}: {maximum}-valued PPM; only 8-bit P6 is read here")
    pixels = data[cursor : cursor + width * height * 3]
    if len(pixels) != width * height * 3:
        raise ValueError(f"{path}: {len(pixels)} pixel byte(s) for a {width}x{height} P6 frame")
    return np.frombuffer(pixels, dtype=np.uint8).reshape(height, width, 3)


@dataclass(frozen=True)
class Comparison:
    """What one pair of frames measures to."""

    mean_delta: np.ndarray  # signed, per channel: rendered minus oracle
    mean_absolute: float
    worst: int
    rendered_std: np.ndarray
    oracle_std: np.ndarray
    regions: np.ndarray  # rows x columns x 3, signed mean delta per region

    @property
    def character(self) -> str:
        """Additive, multiplicative, or neither -- the discriminator this exists for."""
        if self.mean_absolute < 0.5:
            return "matched"
        std_change = float(np.mean(np.abs(self.rendered_std - self.oracle_std)))
        if std_change <= ADDITIVE_STD_TOLERANCE:
            return "additive"
        return "multiplicative"


def compare(rendered: np.ndarray, oracle: np.ndarray, rows: int, columns: int) -> Comparison:
    difference = rendered.astype(np.int16) - oracle.astype(np.int16)
    height, width, _ = rendered.shape
    regions = np.zeros((rows, columns, 3), dtype=np.float64)
    for row in range(rows):
        for column in range(columns):
            top, bottom = row * height // rows, (row + 1) * height // rows
            left, right = column * width // columns, (column + 1) * width // columns
            regions[row, column] = difference[top:bottom, left:right].mean(axis=(0, 1))
    return Comparison(
        mean_delta=difference.mean(axis=(0, 1)),
        mean_absolute=float(np.abs(difference).mean()),
        worst=int(np.abs(difference).max()),
        rendered_std=rendered.astype(np.float64).std(axis=(0, 1)),
        oracle_std=oracle.astype(np.float64).std(axis=(0, 1)),
        regions=regions,
    )


def degenerate(frame: np.ndarray) -> bool:
    return bool(np.max(frame.astype(np.float64).std(axis=(0, 1))) < DEGENERATE_STD)


def report(comparison: Comparison, rows: int, columns: int) -> None:
    channels = " ".join(
        f"{name}{comparison.mean_delta[index]:+.2f}" for index, name in enumerate(CHANNELS)
    )
    print(f"mean delta (rendered - oracle): {channels}")
    print(f"mean |delta| {comparison.mean_absolute:.2f}, worst channel delta {comparison.worst}")
    rendered = " ".join(f"{value:.2f}" for value in comparison.rendered_std)
    oracle = " ".join(f"{value:.2f}" for value in comparison.oracle_std)
    print(f"standard deviation: rendered {rendered} | oracle {oracle}")
    print(f"character: {comparison.character}")
    print(f"per-region mean |delta| ({rows}x{columns}):")
    for row in range(rows):
        cells = " ".join(
            f"{np.abs(comparison.regions[row, column]).mean():6.2f}" for column in range(columns)
        )
        print(f"  {cells}")


def selftest() -> int:
    """Prove the measurement fires, and that it tells the two kinds of delta apart.

    Identity must measure zero: a differ that reported a difference between a frame and itself would
    make every number it produced meaningless. A known additive offset must come back as that offset
    AND be called additive, and a known gain must be called multiplicative -- a classifier that
    answered "additive" to everything would pass the first case alone.
    """
    generator = np.random.default_rng(20260919)
    base = generator.integers(20, 200, size=(64, 96, 3), dtype=np.uint8)
    failures = 0

    identity = compare(base, base, 4, 4)
    if identity.mean_absolute != 0.0 or identity.character != "matched":
        print(f"selftest: a frame against itself measured {identity.mean_absolute}, "
              f"{identity.character}")
        failures += 1
    else:
        print("selftest: a frame against itself measures 0 and is called matched")

    offset = np.clip(base.astype(np.int16) + 24, 0, 255).astype(np.uint8)
    added = compare(offset, base, 4, 4)
    if abs(added.mean_delta.mean() - 24.0) > 0.5 or added.character != "additive":
        print(f"selftest: a +24 offset measured {added.mean_delta.mean():+.2f}, {added.character}")
        failures += 1
    else:
        print(f"selftest: a +24 offset measures {added.mean_delta.mean():+.2f} and is called additive")

    gained = np.clip(base.astype(np.float64) * 1.6, 0, 255).astype(np.uint8)
    scaled = compare(gained, base, 4, 4)
    if scaled.character != "multiplicative":
        print(f"selftest: a 1.6x gain is called {scaled.character}, not multiplicative")
        failures += 1
    else:
        print("selftest: a 1.6x gain is called multiplicative")

    flat = np.zeros((64, 96, 3), dtype=np.uint8)
    if not degenerate(flat) or degenerate(base):
        print("selftest: the degenerate-frame guard does not separate a blank frame from a real one")
        failures += 1
    else:
        print("selftest: a blank frame is refused and a real one is not")

    if failures != 0:
        print("selftest FAILED")
        return 1
    print("selftest PASSED")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("rendered", nargs="?", help="the frame this project produced")
    parser.add_argument("oracle", nargs="?", help="the frame the console produced")
    parser.add_argument("--rows", type=int, default=4)
    parser.add_argument("--columns", type=int, default=4)
    parser.add_argument("--selftest", action="store_true")
    arguments = parser.parse_args()

    if arguments.selftest:
        return selftest()
    if arguments.rendered is None or arguments.oracle is None:
        parser.error("both a rendered frame and an oracle frame are needed")
    if arguments.rows < 1 or arguments.columns < 1:
        parser.error("--rows and --columns count regions, so both are at least 1")

    rendered = read_frame(Path(arguments.rendered))
    oracle = read_frame(Path(arguments.oracle))
    if rendered.shape != oracle.shape:
        sys.exit(
            f"REFUSES: {arguments.rendered} is {rendered.shape[1]}x{rendered.shape[0]} and "
            f"{arguments.oracle} is {oracle.shape[1]}x{oracle.shape[0]}. Resampling one to fit "
            "would measure the resampler."
        )
    blank = [
        name
        for name, frame in ((arguments.rendered, rendered), (arguments.oracle, oracle))
        if degenerate(frame)
    ]
    if blank:
        sys.exit(
            "REFUSES: these frames carry no structure to compare:\n  "
            + "\n  ".join(blank)
            + "\nA diff against a blank frame is a large, confident, meaningless number."
        )

    report(compare(rendered, oracle, arguments.rows, arguments.columns), arguments.rows,
           arguments.columns)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

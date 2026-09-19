#!/usr/bin/env python3
"""Run the gcnport boot diagnostic with every rendering producer attached.

`sunbright_gcnport_boot` takes one flag per guest entry it reads, and a render run needs all of
them. Reconstructing that list by hand is how a run silently renders something other than what the
title draws: omitting `--read-lighting` unlit the scene, and omitting `--read-efb` left the frame
renderer with no pass boundaries at all, so a 256x256 reflection pass was composed straight into the
visible image and a mirrored second Mario floated over the file-select screen for every run that had
ever been taken. Neither omission failed; both produced a frame that looked like a shading defect.

So the producer set is not a thing a caller assembles. It lives here, once, and a run is described
only by what actually varies: what to press, which frame to write, what to watch, how long to run.

    tools/render/boot_run.py --dump-frame scratch/render/fsel.png --dump-frame-index 4500 \
        --pad-script "2900:START 2920:- 3100:START 3120:-" \
        --watch-guest deref:8040dde0+10:6

Every input is refused by name rather than defaulted: a missing executable, a missing DOL, a missing
disc image and a missing memory card are each named individually, because a run that quietly used a
different input is indistinguishable from one that genuinely reached a different boundary.
"""

from __future__ import annotations

import argparse
import os
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path

SELFTEST_REQUIREMENTS: tuple[str, ...] = ()

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "re"))

# Imported after `sys.path` is extended above: `addr2sym` owns the US function list this checks
# against, and duplicating its loader here to avoid the path insert would be a second copy of it.
import addr2sym


@dataclass(frozen=True)
class Producer:
    """One guest entry a render run reads, and the name that address has to resolve to.

    `symbol` is not decoration. These are US GMSE01 addresses typed in by hand, and a wrong digit
    produces a hook on some other function that installs, fires and publishes nothing -- which
    reads exactly like a producer the title never reached. The selftest resolves every one of them
    against the committed function list, so a transcription error fails before a run does.

    It is written exactly as `addr2sym` answers, `name+offset` included. One of these lands inside a
    neighbour because the US function list has no entry of its own for it; pinning the answer that
    list actually gives is what makes the check falsifiable, and writing the name it "should" have
    would make it decorative.

    `symbol` is None for an address that is data rather than code -- `j3dSys` is a global object,
    not an entry point -- and the check then requires that it does NOT resolve to a function.
    """

    flag: str
    address: int
    symbol: str | None
    kind: str = ""

    def arguments(self) -> tuple[str, ...]:
        value = f"{self.address:08x}"
        if self.kind:
            value = f"{self.kind}:{value}"
        if self.flag not in ("--render-frames", "--j3d-sys"):
            value = f"{value}:0"
        return (self.flag, value)


# The producers, in the order a frame is built: the frame seam, then geometry, then what lights it,
# then the 2D pass, then the state that places an immediate-mode draw, then the pass boundaries.
PRODUCERS: tuple[Producer, ...] = (
    Producer("--render-frames", 0x802FC9A4, "waitForRetrace__Q26JDrama6TVideoFUs+0x0"),
    Producer("--read-projections", 0x80362C34, "GXSetProjection+0x0"),
    Producer("--read-models", 0x802E0390, "draw__8J3DShapeCFv+0x0"),
    Producer("--read-lighting", 0x80229A30, "setLight__12TLightCommonFPCQ26JDrama9TGraphicsi+0x0"),
    Producer("--read-lighting", 0x80229610, "setLight__11TLightMarioFPCQ26JDrama9TGraphicsi+0x0"),
    Producer("--read-j2d-screen", 0x802EB6BC, "setup2D__14J2DGrafContextFv+0x0"),
    Producer("--read-pictures", 0x802CC7C0, "drawSelf__10J2DPictureFiiPA3_A4_f+0x0"),
    Producer("--read-windows", 0x802D18EC, "draw_private__9J2DWindowFRC7JUTRectRC7JUTRectPA3_A4_f+0x0"),
    Producer("--read-viewport", 0x80362FAC, "GXSetViewportJitter+0x0", kind="viewport"),
    Producer("--read-rectangles", 0x80140390, "fill_rect__9@unnamed@FRCQ26JDrama5TRectQ28JUtility6TColor+0x0", kind="fade"),
    Producer("--read-rectangles", 0x801400CC, "draw_wipe_box__9@unnamed@FRCQ26JDrama5TRectQ28JUtility6TColor+0x0", kind="wipe"),
    Producer("--read-rectangles", 0x802EBA70, "fillBox__14J2DGrafContextFRC7JUTRect+0x0", kind="fillbox"),
    Producer("--read-matrices", 0x80362E0C, "GXLoadPosMtxImm+0x0", kind="load"),
    Producer("--read-matrices", 0x80362EEC, "GXSetCurrentMtx+0x0", kind="current"),
    Producer("--read-glyphs", 0x802F178C, "setGX__10JUTResFontFv+0x0", kind="setgx"),
    Producer("--read-glyphs", 0x802F1864, "setGX__10JUTResFontFQ28JUtility6TColorQ28JUtility6TColor+0x0", kind="remap"),
    Producer("--read-glyphs", 0x802F1B00, "drawChar_scale__10JUTResFontFffffib+0x0", kind="glyph"),
    # The pass boundaries. Without these the renderer sees no offscreen pass and paints one into
    # the frame the player sees; the file-select screen's 256x256 reflection copy was doing exactly
    # that, 1,424 times a run.
    Producer("--read-efb", 0x8035EE5C, "GXCopyTex+0x0", kind="texture"),
    Producer("--read-efb", 0x8035ECEC, "GXCopyDisp+0x0", kind="display"),
    Producer("--read-efb", 0x8035E388, "GXSetTexCopySrc+0x0", kind="source"),
    Producer("--read-efb", 0x8035EA40, "GXSetCopyClear+0x0", kind="clear"),
    Producer("--press-buttons", 0x80351600, "PADClamp+0xd7c"),
    Producer("--j3d-sys", 0x804045DC, None),
)

DEFAULT_EXECUTABLE = Path("build/tools/gcnport_boot/sunbright_gcnport_boot")
DEFAULT_DOL = Path("scratch/bin/sms.dol")
DEFAULT_MEMORY_CARD = Path("scratch/card/gmse01.raw")
DEFAULT_MAX_BLOCKS = 4_000_000_000


def producer_arguments() -> list[str]:
    """Every producer flag, in build order."""
    arguments: list[str] = []
    for producer in PRODUCERS:
        arguments.extend(producer.arguments())
    return arguments


def build_command(
    *,
    executable: Path,
    dol: Path,
    disc: Path,
    memory_card: Path,
    max_blocks: int,
    dump_frame: Path | None,
    dump_frame_index: int | None,
    pad_script: str | None,
    watches: list[str],
    extra: list[str],
) -> list[str]:
    command = [
        str(executable),
        str(dol),
        "--disc",
        str(disc),
        "--memory-card",
        str(memory_card),
        "--max-blocks",
        str(max_blocks),
    ]
    command.extend(producer_arguments())
    if dump_frame is not None:
        command.extend(("--dump-frame", str(dump_frame)))
    if dump_frame_index is not None:
        command.extend(("--dump-frame-index", str(dump_frame_index)))
    if pad_script is not None:
        command.extend(("--pad-script", pad_script))
    for watch in watches:
        command.extend(("--watch-guest", watch))
    command.extend(extra)
    return command


def _resolve_disc(explicit: str | None) -> Path:
    if explicit is not None:
        return Path(explicit)
    for name in ("SUNBRIGHT_ROM", "SB_ROM"):
        value = os.environ.get(name)
        if value:
            return Path(value)
    sys.exit(
        "REFUSES: no disc image. Pass --disc, or set SUNBRIGHT_ROM (the gitignored .env sets it; "
        "`set -a && . ./.env && set +a`). A render run without the real title's disc reaches the "
        "SDK's disc-error path, which is a different run, not this one."
    )


def _name_of(symbols: list[tuple[int, str]], address: int) -> str | None:
    """`name+offset` for a code address, or None when the address is not inside any function."""
    resolved, _ = addr2sym.resolve(symbols, address)
    if resolved is None:
        return None
    name, offset = resolved
    return f"{name}+{offset:#x}"


def selftest() -> int:
    """Every producer address must name what it claims to, and a wrong one must be caught.

    The positive case is the whole table. The negative is a deliberately misnamed producer fed
    through the same comparison: without it, a check that resolved nothing at all would pass
    silently and report a table it never read.
    """
    symbols = addr2sym.load()
    failures = 0
    for producer in PRODUCERS:
        found = _name_of(symbols, producer.address)
        if found != producer.symbol:
            print(
                f"selftest: {producer.flag} 0x{producer.address:08x} names {found}, "
                f"declared {producer.symbol}"
            )
            failures += 1
    print(f"selftest: {len(PRODUCERS)} producer(s) checked, {failures} misnamed")

    misnamed = Producer("--read-models", 0x802E0390, "definitely_not_this_symbol")
    if _name_of(symbols, misnamed.address) == misnamed.symbol:
        print("selftest: the comparison accepted a knowingly wrong name; it discriminates nothing")
        failures += 1
    else:
        print(f"selftest: a knowingly wrong name for 0x{misnamed.address:08x} is rejected")

    # And the other way round: the data address must not start naming a function, which is what
    # would happen if the function list ever grew past it.
    if _name_of(symbols, 0x804045DC) is not None:
        print("selftest: j3dSys now resolves to a function; the data/code split above is wrong")
        failures += 1

    if failures != 0:
        print("selftest FAILED")
        return 1
    print("selftest PASSED")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--selftest", action="store_true", help="check the producer table and exit")
    parser.add_argument("--print-command", action="store_true", help="print the command, run nothing")
    parser.add_argument("--executable", default=str(DEFAULT_EXECUTABLE))
    parser.add_argument("--dol", default=str(DEFAULT_DOL))
    parser.add_argument("--disc", default=None, help="defaults to $SUNBRIGHT_ROM")
    parser.add_argument("--memory-card", default=str(DEFAULT_MEMORY_CARD))
    parser.add_argument("--max-blocks", type=int, default=DEFAULT_MAX_BLOCKS)
    parser.add_argument("--dump-frame", default=None)
    parser.add_argument("--dump-frame-index", type=int, default=None)
    parser.add_argument("--pad-script", default=None)
    parser.add_argument("--watch-guest", action="append", default=[], dest="watches")
    parser.add_argument("extra", nargs="*", help="further flags passed through unchanged")
    arguments = parser.parse_args()

    if arguments.selftest:
        return selftest()

    executable = Path(arguments.executable)
    dol = Path(arguments.dol)
    memory_card = Path(arguments.memory_card)
    disc = _resolve_disc(arguments.disc)

    missing = [
        f"{label}: {path}"
        for label, path in (
            ("executable", executable),
            ("DOL", dol),
            ("disc image", disc),
        )
        if not path.is_file()
    ]
    # The memory card is where Dolphin WRITES one, not a file that has to be there: it appends the
    # region to the name it is given, so `gmse01.raw` becomes `gmse01.USA.raw` on first use.
    # Its directory does have to exist, and a run that silently saved nowhere is worth refusing.
    if not memory_card.parent.is_dir():
        missing.append(f"memory card directory: {memory_card.parent}")
    if missing and not arguments.print_command:
        sys.exit("REFUSES: these inputs do not exist:\n  " + "\n  ".join(missing))

    if (arguments.dump_frame is None) != (arguments.dump_frame_index is None):
        sys.exit("REFUSES: --dump-frame and --dump-frame-index are given together or not at all.")

    command = build_command(
        executable=executable,
        dol=dol,
        disc=disc,
        memory_card=memory_card,
        max_blocks=arguments.max_blocks,
        dump_frame=Path(arguments.dump_frame) if arguments.dump_frame else None,
        dump_frame_index=arguments.dump_frame_index,
        pad_script=arguments.pad_script,
        watches=arguments.watches,
        extra=arguments.extra,
    )
    if arguments.print_command:
        print(" ".join(command))
        return 0
    return subprocess.run(command, check=False).returncode


if __name__ == "__main__":
    raise SystemExit(main())

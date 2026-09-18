#!/usr/bin/env python3
"""Decode a J3D TEV stage program into the expression it computes.

A `J3DTevStage` is eight bytes: two BP register writes, colour then alpha. Each is a one-byte
register id followed by a 24-bit value. The colour value selects four four-bit inputs; the alpha
value selects four three-bit ones and carries the swap-table indices the colour word does not.
Both then apply the same bias, subtract, clamp, scale and destination.

This exists because the material families are written against these programs, and reading them by
hand does not scale to the scene's materials -- nor does it leave a checkable record. Run
`--selftest` to see it reproduce the programs the shipping families already accept.
"""

import argparse
import sys

COLOR_INPUTS = [
    "prev.rgb", "prev.a", "c0.rgb", "c0.a", "c1.rgb", "c1.a", "c2.rgb", "c2.a",
    "tex.rgb", "tex.a", "ras.rgb", "ras.a", "one", "half", "konst", "zero",
]
ALPHA_INPUTS = ["prev.a", "a0", "a1", "a2", "tex.a", "ras.a", "konst.a", "zero"]
BIASES = ["+0", "+0.5", "-0.5", "?"]
SCALES = ["*1", "*2", "*4", "/2"]
DESTINATIONS = ["prev", "reg0", "reg1", "reg2"]


def _common(value):
    """The fields the colour and alpha words share, in the same bit positions."""
    return {
        "bias": BIASES[(value >> 16) & 3],
        "sub": ((value >> 18) & 1) == 1,
        "clamp": ((value >> 19) & 1) == 1,
        "scale": SCALES[(value >> 20) & 3],
        "dest": DESTINATIONS[(value >> 22) & 3],
    }


def decode_color(value):
    fields = _common(value)
    fields.update(
        a=COLOR_INPUTS[(value >> 12) & 15],
        b=COLOR_INPUTS[(value >> 8) & 15],
        c=COLOR_INPUTS[(value >> 4) & 15],
        d=COLOR_INPUTS[value & 15],
    )
    return fields


def decode_alpha(value):
    fields = _common(value)
    fields.update(
        rswap=value & 3,
        tswap=(value >> 2) & 3,
        a=ALPHA_INPUTS[(value >> 13) & 7],
        b=ALPHA_INPUTS[(value >> 10) & 7],
        c=ALPHA_INPUTS[(value >> 7) & 7],
        d=ALPHA_INPUTS[(value >> 4) & 7],
    )
    return fields


def expression(fields):
    """The TEV combine, written out: dest = d +/- (lerp(a, b, c) + bias), scaled and maybe clamped."""
    a, b, c, d = fields["a"], fields["b"], fields["c"], fields["d"]
    # out = d +/- (lerp(a, b, c) + bias), and the lerp collapses whenever an input is a constant
    # selector. Writing the collapsed form is the point: `lerp(ras.rgb, zero, zero)` and `ras.rgb`
    # are the same program, and only one of them can be compared against a family by eye.
    if c == "zero":
        blend = a
    elif c == "one":
        blend = b
    elif a == b:
        blend = a
    elif a == "zero" and b == "zero":
        blend = "0"
    elif a == "zero":
        blend = f"{b}*{c}"
    elif b == "zero":
        blend = f"{a}*(1-{c})"
    else:
        blend = f"lerp({a}, {b}, {c})"
    terms = "" if d in ("zero",) else d
    sign = " - " if fields["sub"] else " + "
    body = blend if not terms else f"{terms}{sign}{blend}"
    if fields["bias"] != "+0":
        body = f"({body}){fields['bias']}"
    if fields["scale"] != "*1":
        body = f"({body}){fields['scale']}"
    if fields["clamp"]:
        body = f"clamp({body})"
    return f"{fields['dest']} = {body}"


def decode_stage(program):
    """`program` is the eight bytes as they appear in the material."""
    if len(program) != 8:
        raise ValueError(f"a TEV stage is 8 bytes, got {len(program)}")
    color_register, alpha_register = program[0], program[4]
    color = int.from_bytes(program[1:4], "big")
    alpha = int.from_bytes(program[5:8], "big")
    stage = (color_register - 0xC0) // 2
    if color_register != 0xC0 + stage * 2 or alpha_register != color_register + 1:
        raise ValueError(
            f"expected a colour/alpha register pair, got 0x{color_register:02x}/0x{alpha_register:02x}"
        )
    return stage, decode_color(color), decode_alpha(alpha)


def describe(hex_program):
    program = bytes.fromhex(hex_program)
    stage, color, alpha = decode_stage(program)
    return [
        f"stage {stage} colour: {expression(color)}",
        f"stage {stage} alpha:  {expression(alpha)}"
        + (f"  (swap ras={alpha['rswap']} tex={alpha['tswap']})"
           if alpha["rswap"] or alpha["tswap"] else ""),
    ]


# The programs the shipping material families accept, named as those families name them. A decoder
# that cannot reproduce these is wrong about every program it has not been checked against.
SELFTEST = [
    ("c008f8afc108f2f0", "texture times raster", "prev = clamp(tex.rgb*ras.rgb)",
     "prev = clamp(tex.a*ras.a)"),
    ("c008afffc108bff0", "raster pass-through", "prev = clamp(ras.rgb)", "prev = clamp(ras.a)"),
]


def selftest():
    failures = 0
    for hex_program, label, want_color, want_alpha in SELFTEST:
        _, color, alpha = decode_stage(bytes.fromhex(hex_program))
        got_color, got_alpha = expression(color), expression(alpha)
        ok = got_color == want_color and got_alpha == want_alpha
        failures += 0 if ok else 1
        print(f"[{'ok' if ok else 'FAIL'}] {label} ({hex_program})")
        if not ok:
            print(f"       colour: {got_color!r} wanted {want_color!r}")
            print(f"       alpha:  {got_alpha!r} wanted {want_alpha!r}")
    # A decoder that accepts anything proves nothing, so it must also refuse a malformed stage.
    try:
        decode_stage(bytes.fromhex("c008f8afc508f2f0"))
    except ValueError:
        print("[ok] refuses a mismatched register pair")
    else:
        print("[FAIL] accepted a mismatched register pair")
        failures += 1
    return failures


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("programs", nargs="*", help="8-byte TEV stage programs, as hex")
    parser.add_argument("--selftest", action="store_true",
                        help="check the decoder against the programs the families already accept")
    args = parser.parse_args()
    if args.selftest:
        return 1 if selftest() else 0
    if not args.programs:
        parser.error("give at least one program, or --selftest")
    for hex_program in args.programs:
        print(f"{hex_program}:")
        for line in describe(hex_program):
            print(f"  {line}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

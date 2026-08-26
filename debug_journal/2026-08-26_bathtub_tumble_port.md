# `TBathtub::tumble` native decomp port

`TBathtub::tumble(f32 angle, f32 strength)` was still an empty decomp body after
the upstream rebase. The retail US function is GMSE01 `0x801fb568..0x801fb5ec`
(`0x88` bytes).

Ghidra's decompiler and `tools/re/disasm_range.py` agree on the following state
transition:

- `this + 0x29A != 0` returns without writing anything;
- `strength` is multiplied by the SDA2 float at `r2 - 0x1ED8`, whose exact bits
  are `0x38D1B717` (`0.0001f`);
- `angle` is multiplied by the SDA2 float at `r2 - 0x1ED4`, exact bits
  `0x43360B61` (`65536.0f / 360.0f`), truncated, and indexed through JMath's
  short-angle tables;
- `this + 0x1E8` gains `magnitude * JMASSin(short_angle)`;
- `this + 0x1EC` gains the SDA2 value at `r2 - 0x1FB8`, exact `0.0f`;
- `this + 0x1F0` gains `magnitude * -JMASCos(short_angle)`.

The r13 loads corroborate the table identities in `JSystem/JMath.hpp`:
`r13 - 0x5EAC` is `jmaSinShift`, `r13 - 0x5EA8` is `jmaCosTable`, and
`r13 - 0x5EA4` is `jmaSinTable`.

`platform-bathtub_tumble_test` calls the shipping member function with a
four-entry quarter-turn table. Its 0° and 90° cases distinguish sine from cosine
and pin the `0.0001f` scale; its `unk29A = 1` case is the no-mutation control.

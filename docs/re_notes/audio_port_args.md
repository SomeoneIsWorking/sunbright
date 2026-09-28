# Reading the retail audio port arguments

Established 2026-09-28 while reconciling the JAS audio files the upstream
`doldecomp/sms` sync held at our side. The question was narrow: `TPortArgs` has
two unnamed fields at 0x1C (f32) and 0x20 (u32), and the native port's
`sb_portarg_slot` has to map a word index onto them. Everything below is from the
US image, not from the decomp.

## Getting a usable Ghidra project

`scratch/bin/sms.dol` (sha1 `a6782903ef79d4196c8489ecb1b57decb5b3728f`, US
GMSE01 — `0x802e0390` resolves to `draw__8J3DShapeCFv` against
`reference/sms_gmse01_funcs.txt`) is loaded through
`tools/re/DolLoadLocal.py`, not the shared `DolLoad.py`. Two reasons, both
properties of this image rather than of the loader:

- The DOL header's BSS range `0x803E9700..0x8040EB98` **overlaps DATA6**
  (`0x8040C1C0..0x8040CF00`). `createUninitializedBlock` raises
  `MemoryConflictException` and the shared script aborts before mapping
  anything. The local script maps the BSS sub-ranges no data section claims
  (149,336 of 152,728 bytes, 2 blocks) and prints the coverage, so a clipped
  region reads as clipped rather than as covered.
- Ghidra 12.0.4 here has no Jython extension, so the shared `#@runtime Jython`
  scripts cannot run at all. `tools/re/DecompDumpLocal.py` is the same script
  without that header, driven by `pyghidraRun`.

Entry point `0x8000522C`.

## What the retail port-arg writer does

`setSeqPortargsF32__18JAISystemInterfaceFP16JAISeqUpdateDataUlUcf` at
`0x8030D314` (28 bytes) decompiles to a single store:

```c
*(float *)(*(int *)(param_2 + 0x4c) + param_3 * 0x3c + (param_4 & 0xff) * 4 + 4)
    = (float)param_1;
```

`setSeqPortargsU32` at `0x8030D330` is the same store with an `undefined4`.

Read field by field:

- `*(int *)(param_2 + 0x4c)` **dereferences** offset 0x4C of the update struct.
  So `mPlayerParams` is a *pointer* to an array of `JAIPlayerParameter`, and
  `mPlayerParams[track_no]` in the decomp is indexing through it. The upstream
  declaration `JAIPlayerParameter* mPlayerParams;` is the correct one; the two
  are consistent and neither is a bug.
- `param_3 * 0x3c` is the stride: **0x3C = 60 bytes** per `JAIPlayerParameter`.
- `+ 4` skips `mTrack`, then `* 4` per index: the value slots are addressed as
  **32-bit words starting after `mTrack`**, with no field names involved.

That last point is the whole basis of the native port's `sb_portarg_slot`. The
retail arithmetic is only correct when `mTrack` is a 4-byte GameCube pointer. On
a 64-bit host it is 8 bytes, so every index ≥ 1 lands one slot late and the
per-parameter writes scatter into the wrong fields — pitch into volume, pan into
pitch — leaving `mTrackPitch` permanently 0 and the DSP silent. Mapping the index
onto the named field instead is layout-correct on both ABIs, and the retail
offset arithmetic above is the evidence that the slot layout the guard encodes
is the real one.

Worth recording: retail takes its arguments as
`(value, update_struct, track_index, arg_index)` and this decomp declares them
`(update_struct, track_index, arg_index, value)`. The reordering is the
decomp's, not a misread — the offsets above pin every parameter by use.

## TPortArgs 0x1C and 0x20 stay unnamed

The two fields between `mTrackDolby` (0x18) and `mTrackTempo` (0x24) are the
ports this decomp moves but has not identified. What is known:

- `TPortArgs::mArgs` is a 10-element union view (`mArgsAsF32/U32/PS16`) covering
  0x4..0x28, so 0x1C and 0x20 are slots 5 and 6 of that union.
- `TTrack`'s update-flag enum has `UPDATE_Unk5` (0x20) and `UPDATE_FirFilter`
  (0x80) between `UPDATE_Dolby` and the IIR flags, and the decomp carries real
  `setTrackFirU7` / `setTrackFirMultiU7` entry points.

That is suggestive and not sufficient. Naming 0x1C `mFirFilter` because it sits
beside `UPDATE_FirFilter`, or `mUnk5` because the enum counts up to it, would be
inference dressed as evidence, and this decomp's own guide is explicit that a
judgement call without decisive evidence should be left open rather than
committed. They stay `unk1C` / `unk20` until someone has the retail routine that
*consumes* them — the DSP-side writer above only addresses them by offset.

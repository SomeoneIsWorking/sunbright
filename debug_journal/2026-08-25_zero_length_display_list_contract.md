# Zero-length GX display lists are empty, not invalid

## Symptom

After entering Bianco Hills, recomp + Aurora aborted in `gxfifo_drain_pending`:

```
[gxfifo:error] display list 0x8157daa0 +0x0 is outside MEM1
```

The address is inside the GameCube's 24 MiB MEM1. The reported reason was false.

## Root cause

The FIFO hardening in `f9aa668` routed display-list ranges through
`checked_mem1_offset`. That generic helper deliberately rejects a zero-byte memory span, but
`GXCallDisplayList` has different protocol semantics: a zero byte count consumes the nine-byte
call command and executes no nested commands.

This is normal J3D state, not random corruption:

- retail `J3DDisplayListObj::callDL` unconditionally forwards `mpData[0]` and `mSize`;
- constructors and `newDisplayList` initialize `mSize` to zero;
- Nintendo's `GXCallDisplayList` emits the call even when `nbytes` is zero;
- Dolphin's decoder consumes the command and runs zero payload bytes;
- Aurora's direct SDK path writes zero payload bytes;
- `scratch/logs/recomp_test5_z_gxfifo.log`, from a prior successful 1,250-present run, recorded
  both `0x8157daa0 +0x0` and `0x81578640 +0x0` exactly 1,150 times each, then exited cleanly with
  zero amdgpu events.

Before `f9aa668`, the same semantic mistake was hidden by a debug-and-return path. Turning it into
an abort correctly exposed the mismatch, but did not make the old classification correct.

## Fix and controls

`gx_fifo_contracts.hpp` now classifies display-list spans separately as `Empty`, `Valid`, or
`Invalid`. `inline_display_list` returns for `Empty` before nesting checks, pointer arithmetic, or
parsing. Nonempty invalid spans still abort, and nonempty valid spans still require exact parse
consumption.

The focused test contains the exact failing `0x8157daa0 +0` case plus a positive end-of-MEM1 span
and a one-byte-overrun negative control. The generic `checked_mem1_offset(..., 0)` contract remains
unchanged, so texture, TLUT, and other memory-bearing callers do not acquire an empty-span escape.

Verification on 2026-08-25:

- `gx_fifo_contracts`: pass;
- full Clang guest-runtime build: pass;
- `./run-safe.sh SBR_STAGE=1 SBR_QUIT_AFTER=700`: exit 0;
- validated boot-wide amdgpu timeout/reset/fault counter: 42 before, 42 after.

Dolphin also masks call-DL address and size down to 32-byte alignment. The observed address was
already aligned and its encoded size was exactly zero, so that separate fidelity detail neither
caused nor changes this defect.

# GX SDK writers — RE notes & native port (slice 1: the TEV hot set)

Native port: `runtime/overrides/native_gx.cpp`. Census source:
`scratch/logs/call_census.tsv` (dispatched calls per run). Ground truth per
function: GMSE01 disasm (`sunbright-recomp --disasm <addr>`), cross-checked
instruction-for-instruction against `decomp/sms/src/dolphin/gx/GXTev.c` /
`GXBump.c` (decomp matched exactly on every function below) and Dolphin's
`externals/dolphin/Source/Core/VideoCommon/BPMemory.h` for register names.

## Common machinery (VERIFIED in disasm of every function below)

- **`gx` state-block pointer**: SDA load `lwz rX, -0x72F8(r13)` → guest global
  `struct __GXData_struct* gx` (layout: `decomp/sms/src/dolphin/gx/__gx.h`,
  offsets below confirmed in disasm).
- **BP register write sequence** (`GX_WRITE_BP_REG`): byte `0x61`
  (GX_LOAD_BP_REG) then the 32-bit register value, both stored to the
  write-gather pipe `0xCC008000` (`lis 0xCC01; st? -0x8000(r)`). Bridged in
  `runtime/memory_bridge.cpp`: `is_gather_pipe()` routes w8/w16/w32/w64 to
  Dolphin `GPFifo::Write*` — native code reaches it via `sb_w8/sb_w32`.
- **Guest store order**: update shadow word in `__GXData` → `stb 0x61` →
  `stw value` (reloaded from the shadow) → `sth 0, 2(gx)` (`gx->bpSent = 0`,
  u16 at +0x2).
- **Bit numbering** below is LSB=0 ("`n@s`" = n bits at shift s), i.e. the value
  as Dolphin's BPMemory.h bitfields see it; the top byte (8@24) of every BP word
  is the BP register address, pre-seeded into the shadow arrays by `__GXInit`
  and preserved by the RMW writers.

### __GXData offsets used (VERIFIED)

| offset | field | notes |
|---|---|---|
| +0x002 | u16 bpSent | cleared after every BP write |
| +0x100 | u32 tref[8] | BP TREF 0x28+i (two TEV stages per word) |
| +0x130 | u32 tevc[16] | BP TEV_COLOR_ENV 0xC0+2i |
| +0x170 | u32 teva[16] | BP TEV_ALPHA_ENV 0xC1+2i |
| +0x1B0 | u32 tevKsel[8] | BP TEV_KSEL 0xF6+i (two stages per word) |
| +0x49C | u32 texmapId[16] | raw map id incl. the 0x100 "disable" flag |
| +0x4E0 | u32 (decomp `pad0`) | per-stage "texcoord live" bitmask, bit = stage |
| +0x4F4 | u32 dirtyState | TevOrder ORs bit 0 (tref dirty) |

## Functions (slice 1, ~25.8M calls)

### GXSetTevColorIn — 0x8036128C (3,857,525 calls) — VERIFIED
RMW `gx->tevc[stage]` (BPMemory `TevStageCombiner::ColorCombiner`):
fields `a 4@12`, `b 4@8`, `c 4@4`, `d 4@0`. Upper bits (op/bias/scale/clamp/dest
+ address byte 0xC0+2·stage) preserved. One BP write.

### GXSetTevAlphaIn — 0x8036130C (3,857,525) — VERIFIED
RMW `gx->teva[stage]` (`AlphaCombiner`): `a 3@13`, `b 3@10`, `c 3@7`, `d 3@4`.
Bits 0–3 (rswap/tswap) belong to GXSetTevSwapMode and are preserved. One BP write.

### GXSetTevColorOp — 0x80361390 (3,854,323) / GXSetTevAlphaOp — 0x80361450 (3,854,323) — VERIFIED
Identical packing, on `tevc[stage]` / `teva[stage]` respectively:
- `op&1 1@18` (sub bit)
- if `op <= 1` (ADD/SUB): `scale 2@20`, `bias 2@16`
- else (compare ops ≥ GX_TEV_COMP_*): `(op>>1)&3 2@20` (comparison kind),
  `3 2@16` (bias=3 = compare mode marker — disasm `oris r0,r0,3`)
- `clamp&0xFF 1@19`, `out_reg 2@22`
One BP write each.

### GXSetTevSwapMode — 0x80361744 (3,385,376) — VERIFIED
RMW `gx->teva[stage]`: `ras_sel 2@0`, `tex_sel 2@2` (BPMemory rswap/tswap).
One BP write of the alpha-env word.

### GXSetTevKColorSel — 0x8036166C (1,708,273) / GXSetTevKAlphaSel — 0x803616D8 (1,708,273) — VERIFIED
RMW `gx->tevKsel[stage>>1]` (BPMemory `TevKSel`):
- KColorSel: even stage `sel 5@4`, odd stage `sel 5@14`
- KAlphaSel: even stage `sel 5@9`, odd stage `sel 5@19`
(bits 0–3 = the swap-table entries owned by GXSetTevSwapModeTable — preserved.)
One BP write.

### GXSetTevIndirect — 0x80360A18 (3,625,897) — VERIFIED
**No shadow word** — register built from zero (BPMemory `TevStageIndirect`,
BP address `0x10 + tev_stage` packed as `(tev_stage+16) 8@24`):
`ind_stage 2@0`, `format 2@2`, `bias_sel 3@4`, `alpha_sel 2@7`, `matrix_sel 4@9`,
`wrap_s 3@13`, `wrap_t 3@16`, `utc_lod 1@19`, `add_prev 1@20`.
Args 9/10 are on the guest stack (CW EABI): guest reads `lbz 0x33(sp)` /
`lwz 0x34(sp)` after `stwu -0x28` → caller-relative `utc_lod` = byte at SP+0xB
(low byte of the word slot at +0x8), `alpha_sel` = word at SP+0xC.
One BP write + `bpSent=0`; nothing else stored.

### GXSetTevOrder — 0x80361910 (3,858,180) — VERIFIED
The most stateful one:
1. `gx->texmapId[stage] = map` (raw, incl. bit 0x100).
2. `tmap = map & ~0x100; if (tmap >= 8) tmap = 0`.
3. texcoord-live bitmask at gx+0x4E0: `coord >= 8` → clear bit `stage`,
   `tcoord = 0`; else set bit, `tcoord = coord`.
4. `ras = (color == 0xFF /*GX_COLOR_NULL*/) ? 7 : c2r[color]` with
   `c2r[] = {0,1,0,1,0,1,7,5,6}` (guest rodata at 0x803E9390). Caller is assumed
   to pass a valid GXChannelID; other values would index the guest table OOB too.
5. `enable = (map != 0xFF /*GX_TEXMAP_NULL*/ && !(map & 0x100))`.
6. RMW `gx->tref[stage>>1]` (BPMemory `TwoTevStageOrders`, address 0x28+stage/2);
   even stage: `tmap 3@0`, `tcoord 3@3`, `enable 1@6`, `ras 3@7`;
   odd stage: same fields shifted +12 (`3@12`, `3@15`, `1@18`, `3@19`).
7. One BP write, `bpSent=0`, then `gx->dirtyState |= 1`.

## Port notes

- Pure packers `native_gx::pack_*()` have external linkage so the FIFO-shadow
  harness can diff packed values without replaying the overrides.
- Overrides write guest state through `sb_w*` (same byteswap/bridge path as
  recompiled stores) and the FIFO through `sb_w8/sb_w32(0xCC008000, …)`.
- **Coverage caveat**: overrides fire only at dispatch entries, not on
  recomp→recomp direct calls (CLAUDE.md measurement gotcha). Routing the hot
  recomp call sites onto these natives is the main session's harness/integration
  step.

## Not yet ported (next slices, by census)

GXSetTevDirect 0x80360F4C (1,934,781 — RMW `gx->iref` @+0x120),
GXSetTevOp 0x803610E8 (1,802,253 — pure composition of ColorIn/AlphaIn/ColorOp/
AlphaOp, portable as native calls to the slice-1 packers),
GXSetTevColor 0x80361510 / S10 0x80361584 (RA+3×BG write quirk!),
GXSetTevKColor 0x803615F8, GXSetTevSwapModeTable 0x8036179C,
GXSetTevIndWarp 0x80360F94.

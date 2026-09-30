---
id: 42
title: Nineteen functions the decomp itself records as known-wrong
status: open
symptom: 85 objects in `configure.py` are `NonMatching`, and inside the 33 of them that have a source file in this tree there are 19 places where the decomp's own authors left a marker saying the code there is wrong: "this if is completely wrong", "the frame is 8 bytes short here", "fakematch that helps loadArcSeqData", "the references make 0 sense".
state_items: S007
tags: decomp,matching,evidence,blocked
created: 2026-09-29
updated: 2026-09-30
---

## What this is

The decomp's authoritative status data is `configure.py`, which marks every object
`Matching`, `NonMatching` or `Equivalent`. Reading it directly:

    351 Matching
     85 NonMatching
    436 Object() entries

(The 85 is the count from parsing `configure.py` as an AST. An earlier text grep
said 335; that number was wrong and is corrected here.)

The 19 markers below are inside those `NonMatching` objects, at the point where a
reader needs them. They are the decomp speaking about itself, so they are the most
trustworthy work queue in the tree — more trustworthy than any count of `TODO`s,
because a `TODO` is often aspirational and these are conclusions.

    libs/JSystem/src/JAudio/JASystem/JASTrack.cpp:1611
        this if is completely wrong, control flow is crazy here
    libs/JSystem/src/JAudio/JAInterface/JAIBasic.cpp:1467
        the frame is 8 bytes short here; every instruction matches
    libs/JSystem/src/JAudio/JAInterface/JAIBasic.cpp:1357
        fakematch that helps loadArcSeqData
    libs/JSystem/src/J3D/J3DGraphAnimator/J3DModel.cpp:1090
        probably a fakematch, the references make 0 sense
    libs/JSystem/src/JParticle/JPADrawVisitor.cpp:1742, :1767
        try to introduce temps without making this non-matching
    libs/JSystem/src/JParticle/JPAMath.cpp:13
        all these vec math funcs are equivalent (I think), but matching them
    libs/JSystem/src/JParticle/JPAField.cpp:124
        fakematch?
    libs/JSystem/src/J3D/J3DGraphLoader/J3DMaterialFactory_v21.cpp:95
        the frame needs one more 4-byte local here that no instruction
    libs/JSystem/src/JAudio/JAInterface/JAIGFrameStream.cpp:23
        from TWW, might be wrong
    libs/JSystem/src/JAudio/JASystem/JASTrack.cpp:230, :716, :881
        tricky inlines; is this goto real; this is pure pain
    libs/JSystem/src/JAudio/JAInterface/JAIBasic.cpp:261, :268
        // fabricated
    libs/JSystem/src/JKernel/JKRExpHeap.cpp:321
        figure out if ...
    libs/PowerPC_EABI_Support/.../exponentialsf.c:146, :171
        // fabricated; lazy, half assed
    libs/THPPlayer/THPPlayer.c:24
        TODO, figure out if this is a struct

## Why none of it is started here, and why that is not laziness

Every one of these is a *matching* defect, and matching is measurable in exactly one
way: compile with Metrowerks CodeWarrior and diff the object against the original
with objdiff. Three things that requires are absent, and no amount of work in this
environment supplies them:

1. **CodeWarrior** is proprietary, is not present, and is not fetched by the decomp's
   own `tools/download_tool.py` — which does fetch `dtk`, `objdiff-cli`, `wibo` and
   `sjiswrap`, everything open, and nothing proprietary.
2. **The image.** This fork's `decomp/sms/config/` holds GMSJ01 and GMSP01, the
   Japanese and European builds. The only image available is the US one, and region
   is not a small offset, it is a different symbol table. The US disc was listed
   with the Dolphin fork's own `dolphin-tool` and contains **no linker map at all**,
   so `orig/GMSE01/files/marioUS.MAP` — which `tools/validate-symbol-order.py`
   references — cannot be produced from the retail disc either.
3. **A gate that can say no.** This is the part that actually blocks bulk work. A
   recovery whose only check is "it still compiles" accepts code that is plausible
   and wrong, which is the fakematch the decomp's own guide forbids. A swarm without
   an objective gate produces wrong code faster than it can be read.

## What IS available, and was used

- The US image, read through `tools/re/DolLoadLocal.py` and `DecompDumpLocal.py`,
  giving guest-addressed decompilation. This is what named `TResetFruit::unk19c`:
  the ctor at `0x801E1BF4` writes four 16-bit `0xff` stores at +0x19C, and the
  field's only read hands it to `SMS_InitPacket_OneTevColor` with `GX_TEVREG0`.
- `dtk`, fetched through the decomp's own `tools/download_tool.py` with no root, so
  `decomp/sms/tools/symbol_demangle.py` works and any MWCC name can be demangled
  rather than pattern-matched. This is a new capability: previously the decomp's own
  demangler could not run at all.
- `reference/sms_gmse01_funcs.txt` (9,680 US functions) and
  `reference/sms_gmsj01_symbols.txt` (38,262 JP symbols, 37,737 carrying sizes and
  scopes).

## A tool built and then removed

An `unrecovered.py` was written to answer "is there a function in the binary with no
source at all", which is the question underneath everything here. It was removed
rather than shipped. Even after switching from hand-rolled name matching to the
decomp's real demangler it reported 4,130 candidate gaps, of which 1,236 had no
usable identity at all (`@NN@` thunk symbols, `__sinit_*` initialisers) and the bulk
of the rest were template instantiations like `TVec3<float>::set<float>` that the
decomp reconstructs as inline definitions a regex cannot see. A number that is
mostly noise, presented as a gap count, is worse than no number: it invites the
misreading this project keeps recording. The demangling path it established was kept
by fetching `dtk`.

## What a future session can do without the missing pieces

Structure can still be recovered from the US image and checked by reading, which is
how the one rename above happened. What cannot be claimed is byte-identity. So the
reachable work is: for each marker above, decompile the retail function, diff it
against our source, and record where they disagree — as an investigation with a
written report, not as a commit claiming a match.

## Falsifier for this issue's conclusion

That is, an argument that matching work *is* reachable here. It would need a
CodeWarrior build appearing in the tree, or a GMSE01 config with symbols and splits
appearing in `decomp/sms/config/`, or a linker map turning up on the US disc. The
third is already checked and absent; the first two are external state changes.
